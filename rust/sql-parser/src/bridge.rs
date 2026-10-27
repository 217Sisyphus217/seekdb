// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use crate::{ast::*, Parser, SqlMode};

#[cxx::bridge(namespace = "oceanbase::sql::peg")]
mod ffi {
    enum NodeKind {
        Missing,
        Select,
        ProjectList,
        Projection,
        Alias,
        FromList,
        Relation,
        Table,
        Ident,
        Column,
        Star,
        Int,
        Uint,
        Number,
        Double,
        String,
        Bool,
        Null,
        Parameter,
        Parenthesized,
        Unary,
        Binary,
        Where,
        Order,
        SortList,
        SortKey,
        Asc,
        Desc,
        Limit,
        CommaLimit,
        Distinct,
        ExprList,
        Between,
        In,
        Is,
        Function,
        If,
        IsNull,
        CurrentTimestamp,
        CurrentTime,
        CurrentDate,
        UtcTimestamp,
        UtcTime,
        UtcDate,
        Sysdate,
        CurrentUser,
        Count,
        Sum,
        Min,
        Max,
        Avg,
        All,
        Exists,
        JoinedTable,
        InnerJoin,
        LeftJoin,
        RightJoin,
        ColumnList,
        GroupBy,
        WithRollup,
    }

    struct Node {
        kind: NodeKind,
        children: Vec<usize>,
        text: String,
        start: usize,
        end: usize,
        value: u64,
    }

    struct SelectTree {
        nodes: Vec<Node>,
        parameters: usize,
        error: String,
        error_offset: usize,
    }

    extern "Rust" {
        fn parse_select(
            sql: &[u8],
            ansi_quotes: bool,
            no_backslash_escapes: bool,
            pipes_as_concat: bool,
            high_not_precedence: bool,
        ) -> SelectTree;
    }
}

use ffi::{Node, NodeKind as Kind};

struct Encoder {
    nodes: Vec<Node>,
    parameters: usize,
}

impl Encoder {
    fn push(
        &mut self,
        kind: Kind,
        children: Vec<usize>,
        text: String,
        start: usize,
        end: usize,
        value: u64,
    ) -> usize {
        let id = self.nodes.len();
        self.nodes.push(Node {
            kind,
            children,
            text,
            start,
            end,
            value,
        });
        id
    }

    fn branch(&mut self, kind: Kind, children: Vec<usize>) -> usize {
        self.push(kind, children, String::new(), 0, 0, 0)
    }

    fn name(&mut self, name: Name) -> usize {
        self.push(Kind::Ident, vec![], name.text, name.start, name.end, 0)
    }

    fn expression(&mut self, expression: Expr) -> Result<usize, String> {
        let (start, end) = (expression.start, expression.end);
        let mut text = String::new();
        let mut value = 0;
        let mut children = Vec::new();
        let kind = match expression.kind {
            ExprKind::Parenthesized { value } => {
                children.push(self.expression(*value)?);
                Kind::Parenthesized
            }
            ExprKind::Number { text: number } => {
                let kind = if number.contains(['e', 'E']) {
                    Kind::Double
                } else if let Ok(integer) = number.parse::<u64>() {
                    value = integer;
                    if integer <= i64::MAX as u64 {
                        Kind::Int
                    } else {
                        Kind::Uint
                    }
                } else {
                    Kind::Number
                };
                text = number;
                kind
            }
            ExprKind::String { value } => {
                text = value;
                Kind::String
            }
            ExprKind::Boolean { value: boolean } => {
                value = u64::from(boolean);
                Kind::Bool
            }
            ExprKind::Null => Kind::Null,
            ExprKind::Parameter => {
                value = self.parameters as u64;
                self.parameters += 1;
                Kind::Parameter
            }
            ExprKind::Identifier { parts } => {
                if parts.len() > 3 {
                    return Err("column names have at most three parts".into());
                }
                children.resize(3 - parts.len(), 0);
                children.extend(parts.into_iter().map(|name| self.name(name)));
                Kind::Column
            }
            ExprKind::Wildcard { qualifier } => {
                if qualifier.is_empty() {
                    Kind::Star
                } else {
                    if qualifier.len() > 2 {
                        return Err("table qualifiers have at most two parts".into());
                    }
                    children.resize(2 - qualifier.len(), 0);
                    children.extend(qualifier.into_iter().map(|name| self.name(name)));
                    children.push(self.push(Kind::Star, vec![], String::new(), end - 1, end, 0));
                    Kind::Column
                }
            }
            ExprKind::Unary { op, value } => {
                text = op;
                children.push(self.expression(*value)?);
                Kind::Unary
            }
            ExprKind::Binary { op, left, right } => {
                text = op;
                children.push(self.expression(*left)?);
                children.push(self.expression(*right)?);
                Kind::Binary
            }
            ExprKind::Between {
                value: subject,
                low,
                high,
                negated,
            } => {
                value = u64::from(negated);
                children.push(self.expression(*subject)?);
                children.push(self.expression(*low)?);
                children.push(self.expression(*high)?);
                Kind::Between
            }
            ExprKind::In {
                value: subject,
                values,
                query,
                negated,
            } => {
                value = u64::from(negated);
                children.push(self.expression(*subject)?);
                let right = if let Some(query) = query {
                    self.select(*query)?
                } else {
                    let list = values
                        .into_iter()
                        .map(|item| self.expression(item))
                        .collect::<Result<Vec<_>, _>>()?;
                    self.branch(Kind::ExprList, list)
                };
                children.push(right);
                Kind::In
            }
            ExprKind::Is {
                value: subject,
                test,
                negated,
            } => {
                value = u64::from(negated);
                children.push(self.expression(*subject)?);
                children.push(self.expression(*test)?);
                Kind::Is
            }
            ExprKind::Call {
                name,
                distinct,
                arguments,
            } => {
                return self.call(name, distinct, arguments, start, end);
            }
            ExprKind::Subquery { query } => {
                let id = self.select(*query)?;
                self.nodes[id].start = start;
                self.nodes[id].end = end;
                return Ok(id);
            }
            ExprKind::Exists { query } => {
                children.push(self.select(*query)?);
                Kind::Exists
            }
        };
        Ok(self.push(kind, children, text, start, end, value))
    }

    fn call(
        &mut self,
        mut name: Vec<Name>,
        distinct: bool,
        arguments: Vec<Expr>,
        start: usize,
        end: usize,
    ) -> Result<usize, String> {
        if name.len() != 1 {
            return Err("qualified function calls are not integrated yet".into());
        }
        let mut name = name.pop().unwrap();
        let function = if !name.quoted {
            name.text.to_ascii_lowercase()
        } else {
            String::new()
        };
        let kind = match function.as_str() {
            "count" => Kind::Count,
            "sum" => Kind::Sum,
            "min" => Kind::Min,
            "max" => Kind::Max,
            "avg" => Kind::Avg,
            "if" => Kind::If,
            "isnull" => Kind::IsNull,
            "now" | "current_timestamp" | "localtime" | "localtimestamp" => Kind::CurrentTimestamp,
            "curtime" | "current_time" => Kind::CurrentTime,
            "curdate" | "current_date" => Kind::CurrentDate,
            "utc_timestamp" => Kind::UtcTimestamp,
            "utc_time" => Kind::UtcTime,
            "utc_date" => Kind::UtcDate,
            "sysdate" => Kind::Sysdate,
            "current_user" => Kind::CurrentUser,
            _ => Kind::Function,
        };
        if function == "substring" {
            name.text = "substr".into();
        }
        let timestamp = matches!(
            kind,
            Kind::CurrentTimestamp
                | Kind::CurrentTime
                | Kind::UtcTimestamp
                | Kind::UtcTime
                | Kind::Sysdate
        );
        let no_arguments = matches!(kind, Kind::CurrentDate | Kind::UtcDate | Kind::CurrentUser);
        if (kind == Kind::If && arguments.len() != 3)
            || (kind == Kind::IsNull && arguments.len() != 1)
            || (no_arguments && !arguments.is_empty())
        {
            return Err("invalid function argument count".into());
        }
        if timestamp && (arguments.len() > 1 || arguments.first().is_some_and(|arg| {
            !matches!(&arg.kind, ExprKind::Number { text }
                if text.bytes().all(|b| b.is_ascii_digit()) && text.parse::<u8>().is_ok_and(|n| n <= 6))
        })) {
            return Err("time precision must be an integer from 0 to 6".into());
        }
        let aggregate = matches!(
            kind,
            Kind::Count | Kind::Sum | Kind::Min | Kind::Max | Kind::Avg
        );
        let star = arguments
            .iter()
            .any(|arg| matches!(arg.kind, ExprKind::Wildcard { .. }));
        let count_star = kind == Kind::Count
            && !distinct
            && arguments.len() == 1
            && matches!(&arguments[0].kind, ExprKind::Wildcard { qualifier } if qualifier.is_empty());
        if star && !count_star {
            return Err("only COUNT(*) accepts a wildcard argument".into());
        }
        if !aggregate && distinct {
            return Err("DISTINCT requires an aggregate function".into());
        }
        if aggregate
            && !count_star
            && (arguments.is_empty()
                || (!(kind == Kind::Count && distinct) && arguments.len() != 1))
        {
            return Err("invalid aggregate argument count".into());
        }
        let arguments = arguments
            .into_iter()
            .map(|arg| self.expression(arg))
            .collect::<Result<Vec<_>, _>>()?;
        let children = if timestamp || no_arguments {
            vec![arguments.first().copied().unwrap_or(0)]
        } else if !aggregate {
            let name = self.name(name);
            if arguments.is_empty() {
                vec![name]
            } else {
                let args = self.branch(Kind::ExprList, arguments);
                vec![name, args]
            }
        } else if count_star {
            arguments
        } else {
            let modifier = self.branch(if distinct { Kind::Distinct } else { Kind::All }, vec![]);
            let args = if kind == Kind::Count && distinct {
                self.branch(Kind::ExprList, arguments)
            } else {
                arguments[0]
            };
            vec![modifier, args]
        };
        Ok(self.push(kind, children, String::new(), start, end, 0))
    }

    fn relation(&mut self, relation: Relation) -> Result<usize, String> {
        let (source, derived) = match relation.source {
            TableSource::Named { name } => {
                if name.len() > 2 {
                    return Err("table names have at most two parts".into());
                }
                let text = name.last().unwrap().text.clone();
                let mut names = vec![0; 2 - name.len()];
                names.extend(name.into_iter().map(|name| self.name(name)));
                (self.push(Kind::Relation, names, text, 0, 0, 0), false)
            }
            TableSource::Derived { query } => (self.select(*query)?, true),
        };
        if let Some(alias) = relation.alias {
            let text = alias.text.clone();
            let name = self.name(alias);
            let children = if derived {
                vec![source, name]
            } else {
                vec![source, name, 0, 0, 0]
            };
            Ok(self.push(Kind::Alias, children, text, 0, 0, 0))
        } else if derived {
            Err("a derived table requires an alias".into())
        } else {
            Ok(self.branch(Kind::Table, vec![source, 0, 0, 0]))
        }
    }

    fn select(&mut self, query: Select) -> Result<usize, String> {
        let distinct = if query.distinct {
            self.branch(Kind::Distinct, vec![])
        } else {
            0
        };
        let mut projection = Vec::new();
        for item in query.projection {
            let (start, end) = (item.expression.start, item.expression.end);
            if item.alias.is_some() && matches!(item.expression.kind, ExprKind::Wildcard { .. }) {
                return Err("a wildcard cannot have a column alias".into());
            }
            let mut expression = self.expression(item.expression)?;
            if let Some(alias) = item.alias {
                let alias_text = alias.text.clone();
                let name = self.name(alias);
                expression = self.push(
                    Kind::Alias,
                    vec![expression, name],
                    alias_text,
                    start,
                    end,
                    0,
                );
            }
            projection.push(self.push(
                Kind::Projection,
                vec![expression],
                String::new(),
                start,
                end,
                0,
            ));
        }
        let projection = self.branch(Kind::ProjectList, projection);
        let mut tables = Vec::new();
        for table in query.from {
            let mut left = self.relation(table.relation)?;
            for join in table.joins {
                let right = self.relation(join.relation)?;
                let condition = if let Some(condition) = join.condition {
                    self.expression(condition)?
                } else if !join.using.is_empty() {
                    let names = join.using.into_iter().map(|name| self.name(name)).collect();
                    self.branch(Kind::ColumnList, names)
                } else {
                    0
                };
                let kind = self.branch(
                    match join.kind {
                        JoinKind::Left => Kind::LeftJoin,
                        JoinKind::Right => Kind::RightJoin,
                        JoinKind::Inner | JoinKind::Cross => Kind::InnerJoin,
                    },
                    vec![],
                );
                left = self.branch(Kind::JoinedTable, vec![kind, left, right, condition, 0]);
            }
            tables.push(left);
        }
        let from = if tables.is_empty() {
            0
        } else {
            self.branch(Kind::FromList, tables)
        };
        let filter = if let Some(expression) = query.filter {
            let expression = self.expression(expression)?;
            self.branch(Kind::Where, vec![expression, 0])
        } else {
            0
        };
        let mut groups = Vec::new();
        for expression in query.group_by {
            let expression = self.expression(expression)?;
            groups.push(self.branch(Kind::SortKey, vec![expression, 0]));
        }
        let group = if groups.is_empty() {
            0
        } else {
            let list = self.branch(Kind::SortList, groups);
            let rollup = self.branch(Kind::WithRollup, vec![0, list]);
            self.branch(Kind::GroupBy, vec![rollup])
        };
        let having = query
            .having
            .map(|expr| self.expression(expr))
            .transpose()?
            .unwrap_or(0);
        let mut order = Vec::new();
        for item in query.order_by {
            let expression = self.expression(item.expression)?;
            let direction = self.branch(
                if item.descending {
                    Kind::Desc
                } else {
                    Kind::Asc
                },
                vec![],
            );
            order.push(self.branch(Kind::SortKey, vec![expression, direction]));
        }
        let order = if order.is_empty() {
            0
        } else {
            let list = self.branch(Kind::SortList, order);
            self.branch(Kind::Order, vec![list])
        };
        let limit = if let Some(limit) = query.limit {
            // The comma form preserves lexical parameter order: offset, count.
            if limit.comma {
                let offset = self.expression(limit.offset.unwrap())?;
                let count = self.expression(limit.count)?;
                self.branch(Kind::CommaLimit, vec![offset, count])
            } else {
                let count = self.expression(limit.count)?;
                let offset = limit
                    .offset
                    .map(|value| self.expression(value))
                    .transpose()?
                    .unwrap_or(0);
                self.branch(Kind::Limit, vec![count, offset])
            }
        } else {
            0
        };
        Ok(self.branch(
            Kind::Select,
            vec![
                distinct, projection, from, filter, group, having, order, limit,
            ],
        ))
    }
}

fn parse_select(
    sql: &[u8],
    ansi_quotes: bool,
    no_backslash_escapes: bool,
    pipes_as_concat: bool,
    high_not_precedence: bool,
) -> ffi::SelectTree {
    let mut result = ffi::SelectTree {
        nodes: vec![],
        parameters: 0,
        error: String::new(),
        error_offset: 0,
    };
    let sql = match std::str::from_utf8(sql) {
        Ok(sql) => sql,
        Err(error) => {
            result.error_offset = error.valid_up_to();
            result.error = error.to_string();
            return result;
        }
    };
    let mode = SqlMode {
        ansi_quotes,
        no_backslash_escapes,
        pipes_as_concat,
        high_not_precedence,
    };
    stacker::maybe_grow(256 * 1024, 2 * 1024 * 1024, || {
        match Parser::new(mode).parse(sql) {
            Ok(query) => {
                let mut encoder = Encoder {
                    nodes: vec![],
                    parameters: 0,
                };
                encoder.branch(Kind::Missing, vec![]);
                match encoder.select(query) {
                    Ok(_) => {
                        result.nodes = encoder.nodes;
                        result.parameters = encoder.parameters;
                    }
                    Err(error) => result.error = error,
                }
            }
            Err(error) => {
                result.error_offset = error.location.offset;
                result.error = error.to_string();
            }
        }
    });
    result
}
