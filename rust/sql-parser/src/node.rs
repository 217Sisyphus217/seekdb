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

use crate::bridge::ffi;
pub(crate) use ffi::{Node, NodeKind as Kind};

pub(crate) const MAX_DEPTH: usize = 128;

pub(crate) struct Name {
    pub text: String,
    pub quoted: bool,
    pub start: usize,
    pub end: usize,
}

#[derive(Default)]
pub(crate) struct Select {
    pub distinct: bool,
    pub projection: Vec<Node>,
    pub from: Vec<Node>,
    pub filter: Option<Node>,
    pub group_by: Vec<Node>,
    pub having: Option<Node>,
    pub order_by: Vec<Node>,
    pub limit: Option<Node>,
    pub lock: Option<Node>,
    pub hints: Vec<Node>,
    pub windows: Vec<Node>,
}

impl Node {
    pub fn setting_value(variable: &Self, mut value: Self) -> Self {
        if variable.kind != Kind::UserVariable
            && value.kind == Kind::Column
            && value.children[0].kind == Kind::Missing
            && value.children[1].kind == Kind::Missing
        {
            let name = value.children.pop().unwrap();
            value = Self::new(Kind::String, vec![])
                .text(name.text)
                .at(value.start, value.end);
        }
        value
    }

    pub fn optional(node: Option<Self>) -> Self {
        node.unwrap_or_else(Self::missing)
    }

    pub fn list(kind: Kind, children: Vec<Self>) -> Self {
        if children.is_empty() {
            Self::missing()
        } else {
            Self::new(kind, children)
        }
    }

    pub fn where_clause(value: Option<Self>) -> Self {
        Self::optional(value.map(|value| Self::new(Kind::Where, vec![value, Self::missing()])))
    }

    pub fn order_clause(values: Vec<Self>) -> Self {
        if values.is_empty() {
            Self::missing()
        } else {
            Self::new(Kind::Order, vec![Self::new(Kind::SortList, values)])
        }
    }

    pub fn insert(
        replace: bool,
        ignore: bool,
        table: Self,
        columns: Vec<Self>,
        values: Self,
        assignments: Vec<Self>,
        hints: Vec<Self>,
    ) -> Self {
        let into = Self::new(
            Kind::InsertInto,
            vec![table, Self::list(Kind::ColumnList, columns)],
        );
        let insert = Self::new(
            Kind::SingleTableInsert,
            vec![
                into,
                values,
                Self::list(Kind::AssignList, assignments),
                Self::missing(),
            ],
        );
        Self::new(
            Kind::Insert,
            vec![
                insert,
                Self::new(if replace { Kind::Replace } else { Kind::Insert }, vec![]),
                Self::list(Kind::Hints, hints),
                if ignore {
                    Self::new(Kind::Ignore, vec![])
                } else {
                    Self::missing()
                },
            ],
        )
    }

    pub fn data_type(
        name: &str,
        parameters: Vec<Self>,
        unsigned: bool,
        zerofill: bool,
        start: usize,
        end: usize,
    ) -> Result<Self, &'static str> {
        let numeric = matches!(
            name,
            "tinyint"
                | "smallint"
                | "mediumint"
                | "int"
                | "bigint"
                | "float"
                | "double"
                | "decimal"
        );
        let max_parameters = if matches!(name, "float" | "double" | "decimal") {
            2
        } else {
            1
        };
        if parameters.len() > max_parameters
            || parameters
                .iter()
                .any(|parameter| !matches!(parameter.kind, Kind::Int | Kind::Uint))
            || (name == "double" && parameters.len() == 1)
            || (matches!(name, "varchar" | "varbinary") && parameters.is_empty())
            || (matches!(name, "bool" | "boolean" | "date" | "json") && !parameters.is_empty())
            || (!numeric && (unsigned || zerofill))
        {
            return Err("invalid data type parameters");
        }
        Ok(Self::new(Kind::DataType, parameters)
            .text(name.into())
            .value(u64::from(unsigned) | (u64::from(zerofill) << 1))
            .at(start, end))
    }

    pub fn new(kind: Kind, children: Vec<Self>) -> Self {
        let depth = children.iter().map(|child| child.depth).max().unwrap_or(0);
        Self {
            kind,
            children,
            text: String::new(),
            start: 0,
            end: 0,
            value: 0,
            depth,
        }
    }

    pub fn missing() -> Self {
        Self::new(Kind::Missing, vec![])
    }

    pub fn text(mut self, text: String) -> Self {
        self.text = text;
        self
    }
    pub fn value(mut self, value: u64) -> Self {
        self.value = value;
        self
    }
    pub fn at(mut self, start: usize, end: usize) -> Self {
        self.start = start;
        self.end = end;
        self
    }

    pub fn expression(mut self, start: usize, end: usize) -> Result<Self, &'static str> {
        if self.depth >= MAX_DEPTH {
            return Err("expression depth below 128 levels");
        }
        self.depth += 1;
        Ok(self.at(start, end))
    }

    pub fn name(name: Name) -> Self {
        Self::new(Kind::Ident, vec![])
            .text(name.text)
            .at(name.start, name.end)
    }

    pub fn number(text: &str, start: usize, end: usize) -> Result<Self, &'static str> {
        let mut value = 0;
        let kind = if text.contains(['e', 'E']) {
            Kind::Double
        } else if let Ok(integer) = text.parse::<u64>() {
            value = integer;
            if integer <= i64::MAX as u64 {
                Kind::Int
            } else {
                Kind::Uint
            }
        } else {
            Kind::Number
        };
        Self::new(kind, vec![])
            .text(text.into())
            .value(value)
            .expression(start, end)
    }

    pub fn column(
        names: Vec<Name>,
        star: bool,
        start: usize,
        end: usize,
    ) -> Result<Self, &'static str> {
        let count = names.len() + usize::from(star);
        if count > 3 {
            return Err("column names have at most three parts");
        }
        let mut children: Vec<_> = (count..3).map(|_| Self::missing()).collect();
        children.extend(names.into_iter().map(Self::name));
        if star {
            children.push(Self::new(Kind::Star, vec![]).at(end - 1, end));
        }
        Self::new(Kind::Column, children).expression(start, end)
    }

    pub fn unary(op: &str, value: Self, start: usize) -> Result<Self, &'static str> {
        let end = value.end;
        if op == "binary" {
            let target = Self::new(Kind::CastType, vec![])
                .text("varbinary".into())
                .at(start, start + 6);
            return Self::call(
                vec![Name {
                    text: "cast".into(),
                    quoted: false,
                    start,
                    end: start + 6,
                }],
                false,
                vec![value, target],
                None,
                start,
                end,
            );
        }
        Self::new(Kind::Unary, vec![value])
            .text(op.into())
            .expression(start, end)
    }

    pub fn binary(op: String, left: Self, right: Self) -> Result<Self, &'static str> {
        let (start, end) = (left.start, right.end);
        Self::new(Kind::Binary, vec![left, right])
            .text(op)
            .expression(start, end)
    }

    fn is_wildcard(&self) -> bool {
        self.kind == Kind::Star
            || (self.kind == Kind::Column
                && self
                    .children
                    .last()
                    .is_some_and(|child| child.kind == Kind::Star))
    }

    pub fn call(
        mut name: Vec<Name>,
        distinct: bool,
        arguments: Vec<Self>,
        window: Option<Self>,
        start: usize,
        end: usize,
    ) -> Result<Self, &'static str> {
        if name.len() != 1 {
            return Err("qualified function calls are not integrated yet");
        }
        let mut name = name.pop().unwrap();
        let function = if !name.quoted {
            name.text.to_ascii_lowercase()
        } else {
            String::new()
        };
        if function == "mod" {
            if window.is_some() || distinct || arguments.len() != 2 {
                return Err("MOD requires two arguments");
            }
            return Self::new(Kind::Binary, arguments)
                .text("mod".into())
                .expression(start, end);
        }
        let kind = match function.as_str() {
            "count" => Kind::Count,
            "sum" => Kind::Sum,
            "min" => Kind::Min,
            "max" => Kind::Max,
            "avg" => Kind::Avg,
            "row_number" => Kind::RowNumber,
            "rank" => Kind::Rank,
            "dense_rank" => Kind::DenseRank,
            "percent_rank" => Kind::PercentRank,
            "cume_dist" => Kind::CumeDist,
            "ntile" => Kind::Ntile,
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
        } else if function == "schema" {
            name.text = "database".into();
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
            return Err("invalid function argument count");
        }
        if timestamp
            && (arguments.len() > 1
                || arguments.first().is_some_and(|arg| {
                    !(matches!(arg.kind, Kind::Int | Kind::Uint) && arg.value <= 6)
                }))
        {
            return Err("time precision must be an integer from 0 to 6");
        }
        let aggregate = matches!(
            kind,
            Kind::Count | Kind::Sum | Kind::Min | Kind::Max | Kind::Avg
        );
        let ranking = matches!(
            kind,
            Kind::RowNumber
                | Kind::Rank
                | Kind::DenseRank
                | Kind::PercentRank
                | Kind::CumeDist
                | Kind::Ntile
        );
        if (ranking && (window.is_none() || arguments.len() != usize::from(kind == Kind::Ntile)))
            || (window.is_some() && !ranking && !aggregate)
        {
            return Err("invalid window function");
        }
        let star = arguments.iter().any(|arg| arg.is_wildcard());
        let count_star = kind == Kind::Count
            && !distinct
            && arguments.len() == 1
            && arguments[0].kind == Kind::Star;
        if star && !count_star {
            return Err("only COUNT(*) accepts a wildcard argument");
        }
        if !aggregate && distinct {
            return Err("DISTINCT requires an aggregate function");
        }
        if aggregate
            && !count_star
            && (arguments.is_empty()
                || (!(kind == Kind::Count && distinct) && arguments.len() != 1))
        {
            return Err("invalid aggregate argument count");
        }
        let children = if ranking {
            arguments
        } else if timestamp || no_arguments {
            vec![arguments.into_iter().next().unwrap_or_else(Self::missing)]
        } else if !aggregate {
            let name = Self::name(name);
            if arguments.is_empty() {
                vec![name]
            } else {
                vec![name, Self::new(Kind::ExprList, arguments)]
            }
        } else if count_star {
            arguments
        } else {
            let modifier = Self::new(if distinct { Kind::Distinct } else { Kind::All }, vec![]);
            let args = if kind == Kind::Count && distinct {
                Self::new(Kind::ExprList, arguments)
            } else {
                arguments.into_iter().next().unwrap()
            };
            vec![modifier, args]
        };
        let function = Self::new(kind, children);
        match window {
            Some(window) => Self::new(Kind::WindowFunction, vec![function, window]),
            None => function,
        }
        .expression(start, end)
    }

    pub fn relation(names: Vec<Name>) -> Result<Self, &'static str> {
        if names.len() > 2 {
            return Err("table names have at most two parts");
        }
        let text = names.last().unwrap().text.clone();
        let mut children: Vec<_> = (names.len()..2).map(|_| Self::missing()).collect();
        children.extend(names.into_iter().map(Self::name));
        Ok(Self::new(Kind::Relation, children).text(text))
    }

    pub fn table(source: Self, alias: Option<Name>) -> Result<Self, &'static str> {
        let derived = source.kind == Kind::Select;
        if let Some(alias) = alias {
            let text = alias.text.clone();
            let mut children = vec![source, Self::name(alias)];
            if !derived {
                children.extend((0..3).map(|_| Self::missing()));
            }
            Ok(Self::new(Kind::Alias, children).text(text))
        } else if derived {
            Ok(Self::new(
                Kind::Alias,
                vec![source, Self::new(Kind::Ident, vec![])],
            ))
        } else {
            Ok(Self::new(
                Kind::Table,
                vec![source, Self::missing(), Self::missing(), Self::missing()],
            ))
        }
    }

    pub fn projection(mut value: Self, alias: Option<Self>) -> Result<Self, &'static str> {
        let (start, end) = (value.start, value.end);
        if let Some(mut alias) = alias {
            if value.is_wildcard() {
                return Err("a wildcard cannot have a column alias");
            }
            let text = alias.text.clone();
            let string = alias.kind == Kind::String;
            let span = if string {
                (alias.start, alias.end)
            } else {
                (start, end)
            };
            alias.kind = Kind::Ident;
            value = Self::new(
                if string {
                    Kind::StringAlias
                } else {
                    Kind::Alias
                },
                vec![value, alias],
            )
            .text(text)
            .at(span.0, span.1);
        }
        Ok(Self::new(Kind::Projection, vec![value]).at(start, end))
    }

    pub fn select(query: Select) -> Result<Self, &'static str> {
        let distinct = if query.distinct {
            Self::new(Kind::Distinct, vec![])
        } else {
            Self::missing()
        };
        let projection = Self::new(Kind::ProjectList, query.projection);
        let from = if query.from.is_empty() {
            Self::missing()
        } else {
            Self::new(Kind::FromList, query.from)
        };
        let filter = query
            .filter
            .map(|node| Self::new(Kind::Where, vec![node, Self::missing()]))
            .unwrap_or_else(Self::missing);
        let group = if query.group_by.is_empty() {
            Self::missing()
        } else {
            let keys = query
                .group_by
                .into_iter()
                .map(|node| Self::new(Kind::SortKey, vec![node, Self::missing()]))
                .collect();
            let list = Self::new(Kind::SortList, keys);
            Self::new(
                Kind::GroupBy,
                vec![Self::new(Kind::WithRollup, vec![Self::missing(), list])],
            )
        };
        let having = query.having.unwrap_or_else(Self::missing);
        let order = if query.order_by.is_empty() {
            Self::missing()
        } else {
            Self::new(Kind::Order, vec![Self::new(Kind::SortList, query.order_by)])
        };
        Self::new(
            Kind::Select,
            vec![
                distinct,
                projection,
                from,
                filter,
                group,
                having,
                Self::list(Kind::NamedWindows, query.windows),
                order,
                query.limit.unwrap_or_else(Self::missing),
                query.lock.unwrap_or_else(Self::missing),
                Self::list(Kind::Hints, query.hints),
            ],
        )
        .expression(0, 0)
    }
}
