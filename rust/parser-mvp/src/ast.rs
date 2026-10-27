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

use serde::Serialize;

pub(crate) const MAX_DEPTH: usize = 128;

#[derive(Debug, PartialEq, Serialize)]
pub struct Select {
    pub distinct: bool,
    pub projection: Vec<SelectItem>,
    pub from: Vec<Table>,
    pub filter: Option<Expr>,
    pub group_by: Vec<Expr>,
    pub having: Option<Expr>,
    pub order_by: Vec<OrderItem>,
    pub limit: Option<Limit>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct SelectItem {
    pub expression: Expr,
    pub alias: Option<Name>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Table {
    pub relation: Relation,
    pub joins: Vec<Join>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Relation {
    pub source: TableSource,
    pub alias: Option<Name>,
}

#[derive(Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum TableSource {
    Named { name: Vec<Name> },
    Derived { query: Box<Select> },
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Join {
    pub kind: String,
    pub relation: Relation,
    pub condition: Option<Expr>,
    pub using: Vec<Name>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct OrderItem {
    pub expression: Expr,
    pub descending: bool,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Limit {
    pub count: Expr,
    pub offset: Option<Expr>,
    pub comma: bool,
}

#[derive(Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ExprKind {
    Parenthesized {
        value: Box<Expr>,
    },
    Identifier {
        parts: Vec<Name>,
    },
    Number {
        text: String,
    },
    String {
        value: String,
    },
    Boolean {
        value: bool,
    },
    Null,
    Parameter,
    Wildcard {
        qualifier: Vec<Name>,
    },
    Unary {
        op: String,
        value: Box<Expr>,
    },
    Binary {
        op: String,
        left: Box<Expr>,
        right: Box<Expr>,
    },
    Call {
        name: Vec<Name>,
        distinct: bool,
        arguments: Vec<Expr>,
    },
    Between {
        value: Box<Expr>,
        low: Box<Expr>,
        high: Box<Expr>,
        negated: bool,
    },
    In {
        value: Box<Expr>,
        values: Vec<Expr>,
        query: Option<Box<Select>>,
        negated: bool,
    },
    Is {
        value: Box<Expr>,
        test: Box<Expr>,
        negated: bool,
    },
    Subquery {
        query: Box<Select>,
    },
    Exists {
        query: Box<Select>,
    },
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Name {
    pub text: String,
    pub start: usize,
    pub end: usize,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Expr {
    #[serde(skip)]
    depth: usize,
    #[serde(flatten)]
    pub kind: ExprKind,
    pub start: usize,
    pub end: usize,
}

impl Expr {
    pub fn new(kind: ExprKind, start: usize, end: usize) -> Result<Self, &'static str> {
        let child_depth = match &kind {
            ExprKind::Parenthesized { value } | ExprKind::Unary { value, .. } => value.depth,
            ExprKind::Binary { left, right, .. } => left.depth.max(right.depth),
            ExprKind::Between {
                value, low, high, ..
            } => value.depth.max(low.depth).max(high.depth),
            ExprKind::Is { value, test, .. } => value.depth.max(test.depth),
            ExprKind::Call { arguments, .. } => {
                arguments.iter().map(|value| value.depth).max().unwrap_or(0)
            }
            ExprKind::In {
                value,
                values,
                query,
                ..
            } => value
                .depth
                .max(values.iter().map(|value| value.depth).max().unwrap_or(0))
                .max(query.as_ref().map(|query| query.depth()).unwrap_or(0)),
            ExprKind::Subquery { query } | ExprKind::Exists { query } => query.depth(),
            _ => 0,
        };
        if child_depth >= MAX_DEPTH {
            return Err("expression depth below 128 levels");
        }
        Ok(Self {
            kind,
            start,
            end,
            depth: child_depth + 1,
        })
    }
}

impl Select {
    fn depth(&self) -> usize {
        self.projection
            .iter()
            .map(|item| item.expression.depth)
            .chain(self.filter.iter().map(|value| value.depth))
            .chain(self.group_by.iter().map(|value| value.depth))
            .chain(self.having.iter().map(|value| value.depth))
            .chain(self.order_by.iter().map(|item| item.expression.depth))
            .chain(
                self.from
                    .iter()
                    .flat_map(|table| &table.joins)
                    .filter_map(|join| join.condition.as_ref())
                    .map(|value| value.depth),
            )
            .chain(
                self.from
                    .iter()
                    .flat_map(|table| {
                        std::iter::once(&table.relation)
                            .chain(table.joins.iter().map(|join| &join.relation))
                    })
                    .map(|relation| match &relation.source {
                        TableSource::Named { .. } => 0,
                        TableSource::Derived { query } => query.depth(),
                    }),
            )
            .max()
            .unwrap_or(0)
            + 1
    }
}
