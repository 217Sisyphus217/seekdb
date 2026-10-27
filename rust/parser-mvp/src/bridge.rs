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
                query: None,
                negated,
            } => {
                value = u64::from(negated);
                children.push(self.expression(*subject)?);
                let list = values
                    .into_iter()
                    .map(|item| self.expression(item))
                    .collect::<Result<Vec<_>, _>>()?;
                children.push(self.branch(Kind::ExprList, list));
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
            _ => return Err("this SELECT expression is not integrated yet".into()),
        };
        Ok(self.push(kind, children, text, start, end, value))
    }

    fn select(&mut self, query: Select) -> Result<(), String> {
        if !query.group_by.is_empty() || query.having.is_some() {
            return Err("GROUP BY and HAVING are not integrated yet".into());
        }
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
            if !table.joins.is_empty() {
                return Err("JOIN is not integrated yet".into());
            }
            let TableSource::Named { name } = table.relation.source else {
                return Err("derived tables are not integrated yet".into());
            };
            if name.len() > 2 {
                return Err("table names have at most two parts".into());
            }
            let text = name.last().unwrap().text.clone();
            let mut names = vec![0; 2 - name.len()];
            names.extend(name.into_iter().map(|name| self.name(name)));
            let relation = self.push(Kind::Relation, names, text, 0, 0, 0);
            if let Some(alias) = table.relation.alias {
                let text = alias.text.clone();
                let name = self.name(alias);
                tables.push(self.push(Kind::Alias, vec![relation, name, 0, 0, 0], text, 0, 0, 0));
            } else {
                tables.push(self.branch(Kind::Table, vec![relation, 0, 0, 0]));
            }
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
        self.branch(
            Kind::Select,
            vec![distinct, projection, from, filter, order, limit],
        );
        Ok(())
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
                    Ok(()) => {
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
