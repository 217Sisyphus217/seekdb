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

pub(crate) struct Select {
    pub distinct: bool,
    pub projection: Vec<Node>,
    pub from: Vec<Node>,
    pub filter: Option<Node>,
    pub group_by: Vec<Node>,
    pub having: Option<Node>,
    pub order_by: Vec<Node>,
    pub limit: Option<Node>,
}

impl Node {
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
        let children = if timestamp || no_arguments {
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
        Self::new(kind, children).expression(start, end)
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
            Err("a derived table requires an alias")
        } else {
            Ok(Self::new(
                Kind::Table,
                vec![source, Self::missing(), Self::missing(), Self::missing()],
            ))
        }
    }

    pub fn projection(mut value: Self, alias: Option<Name>) -> Result<Self, &'static str> {
        let (start, end) = (value.start, value.end);
        if let Some(alias) = alias {
            if value.is_wildcard() {
                return Err("a wildcard cannot have a column alias");
            }
            let text = alias.text.clone();
            value = Self::new(Kind::Alias, vec![value, Self::name(alias)])
                .text(text)
                .at(start, end);
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
                order,
                query.limit.unwrap_or_else(Self::missing),
            ],
        )
        .expression(0, 0)
    }
}
