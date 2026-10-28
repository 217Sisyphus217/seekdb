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
    pub with: Option<Node>,
    pub parenthesized: bool,
    pub set: Option<Node>,
    pub options: Vec<Node>,
    pub projection: Vec<Node>,
    pub into: Option<Node>,
    pub into_end: Option<Node>,
    pub approximate: Option<Node>,
    pub vector_parameters: Option<Node>,
    pub from: Vec<Node>,
    pub filter: Option<Node>,
    pub group_by: Option<Node>,
    pub having: Option<Node>,
    pub order_by: Vec<Node>,
    pub limit: Option<Node>,
    pub lock: Option<Node>,
    pub hints: Vec<Node>,
    pub windows: Vec<Node>,
}

impl Select {
    pub fn into_cte(
        mut self,
        name: Name,
        columns: Vec<Name>,
        recursive: bool,
    ) -> Result<Node, &'static str> {
        if recursive {
            if let Some(set) = &mut self.set {
                if set.children.len() > 2 {
                    let right = set.children.pop().unwrap();
                    let left = Node::select(Self {
                        set: Some(Node::new(set.kind, std::mem::take(&mut set.children))),
                        ..Self::default()
                    })?;
                    set.children = vec![left, right];
                }
            }
        }
        Ok(Node::new(
            Kind::CommonTable,
            vec![
                Node::name(name),
                Node::list(
                    Kind::ColumnList,
                    columns.into_iter().map(Node::name).collect(),
                ),
                Node::select(self)?,
            ],
        ))
    }

    pub fn combine(self, kind: Kind, right: Self) -> Result<Self, &'static str> {
        let right = Node::select(right)?;
        let flatten = matches!(kind, Kind::Union | Kind::UnionAll)
            && !self.parenthesized
            && self.order_by.is_empty()
            && self.limit.is_none()
            && self.set.as_ref().is_some_and(|set| set.kind == kind);
        let children = if flatten {
            let mut children = self.set.unwrap().children;
            children.push(right);
            children
        } else {
            vec![Node::select(self)?, right]
        };
        Ok(Self {
            set: Some(Node::new(kind, children)),
            ..Self::default()
        })
    }

    pub fn finish(
        mut self,
        order: Vec<Node>,
        limit: Option<Node>,
        lock: Option<Node>,
        into: Option<Node>,
        approximate: Option<Node>,
        vector_parameters: Option<Node>,
    ) -> Result<Self, &'static str> {
        if approximate.is_some()
            && (order.is_empty()
                || limit.is_none()
                || self.from.is_empty()
                || self.parenthesized
                || self.set.is_some())
        {
            return Err("APPROX requires a simple table query with ORDER BY and LIMIT");
        }
        if self.parenthesized && (!order.is_empty() || limit.is_some()) {
            self = Self {
                projection: vec![Node::new(
                    Kind::Projection,
                    vec![Node::new(Kind::Star, vec![])],
                )],
                from: vec![Node::table(Node::select(self)?, None)?],
                ..Self::default()
            };
        }
        if !order.is_empty() {
            self.order_by = order;
        }
        self.limit = limit.or(self.limit);
        self.lock = lock.or(self.lock);
        self.into_end = into.or(self.into_end);
        self.approximate = approximate.or(self.approximate);
        self.vector_parameters = vector_parameters.or(self.vector_parameters);
        Ok(self)
    }
}

impl Node {
    fn contains_user_variable(&self) -> bool {
        matches!(self.kind, Kind::GetUserVariable | Kind::UserAssignment)
            || self.children.iter().any(Self::contains_user_variable)
    }

    pub fn pl_expression(self) -> Result<Self, &'static str> {
        if !self.contains_user_variable() {
            return Ok(self);
        }
        let (start, end) = (self.start, self.end);
        let alias = Self::new(Kind::Ident, vec![]).text("SUBQUERY".into());
        let projection = Self::new(
            Kind::Projection,
            vec![Self::new(Kind::Alias, vec![self, alias])],
        );
        let mut query = Self::select(Select {
            projection: vec![projection],
            ..Select::default()
        })?;
        query.kind = Kind::PlSubquery;
        Ok(query.at(start, end))
    }

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
            || (matches!(name, "bool" | "boolean" | "date" | "json" | "geometry")
                && !parameters.is_empty())
            || (matches!(
                name,
                "tinytext"
                    | "tinyblob"
                    | "mediumtext"
                    | "mediumblob"
                    | "longtext"
                    | "longblob"
                    | "string"
            ) && !parameters.is_empty())
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
                None,
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

    pub fn is_wildcard(&self) -> bool {
        self.kind == Kind::Star
            || (self.kind == Kind::Column
                && self
                    .children
                    .last()
                    .is_some_and(|child| child.kind == Kind::Star))
    }

    pub fn cast_array(
        mut self,
        target: Self,
        start: usize,
        end: usize,
    ) -> Result<Self, &'static str> {
        while self.kind == Kind::Parenthesized {
            self = self.children.pop().unwrap();
        }
        if self.kind == Kind::Function
            && self.children[0].text.eq_ignore_ascii_case("json_unquote")
            && self.children[1].children.len() == 1
        {
            self = self.children.pop().unwrap().children.pop().unwrap();
            while self.kind == Kind::Parenthesized {
                self = self.children.pop().unwrap();
            }
        }
        let mut children = if self.kind == Kind::Column {
            vec![self, Self::new(Kind::HiddenString, vec![])]
        } else if self.kind == Kind::Function
            && self.children[0].text.eq_ignore_ascii_case("json_extract")
            && self.children[1].children.len() == 2
        {
            self.children.pop().unwrap().children
        } else {
            return Err("CAST ARRAY requires a JSON column or extracted path");
        };
        children.push(target);
        // JSON_QUERY: truncate, scalars, pretty, ascii, wrapper, asis, error, empty, mismatch, multivalue.
        children.extend(
            [0, 2, 0, 0, 1, 1, 0, 1, 1, 0]
                .into_iter()
                .map(|value| Self::new(Kind::HiddenInt, vec![]).value(value)),
        );
        Self::new(Kind::JsonQuery, children).expression(start, end)
    }

    pub fn call(
        mut name: Vec<Name>,
        modifier: Option<Kind>,
        arguments: Vec<Self>,
        window: Option<Self>,
        start: usize,
        end: usize,
    ) -> Result<Self, &'static str> {
        if name.len() > 1 {
            if name.len() > 3 || modifier.is_some() || window.is_some() {
                return Err("qualified function call");
            }
            let function = Self::new(
                Kind::Function,
                vec![
                    Self::name(name.pop().unwrap()),
                    Self::list(Kind::ExprList, arguments),
                ],
            );
            let mut access = Self::new(Kind::ObjectAccess, vec![function, Self::missing()]);
            for part in name.into_iter().rev() {
                access = Self::new(Kind::ObjectAccess, vec![Self::name(part), access]);
            }
            return access.expression(start, end);
        }
        let mut name = name.pop().unwrap();
        let distinct = modifier == Some(Kind::Distinct);
        let function = if !name.quoted {
            name.text.to_ascii_lowercase()
        } else {
            String::new()
        };
        if function == "cast" && (arguments.len() != 2 || arguments[1].kind != Kind::CastType) {
            return Err("CAST requires an expression and AS type");
        }
        if matches!(
            function.as_str(),
            "first_value" | "last_value" | "nth_value" | "lead" | "lag"
        ) {
            return Err("window function requires OVER");
        }
        if matches!(
            function.as_str(),
            "date_add"
                | "date_sub"
                | "adddate"
                | "subdate"
                | "timestampadd"
                | "timestampdiff"
                | "extract"
        ) {
            return Err("date function syntax");
        }
        if function == "mod" {
            if window.is_some() || modifier.is_some() || arguments.len() != 2 {
                return Err("MOD requires two arguments");
            }
            return Self::new(Kind::Binary, arguments)
                .text("mod".into())
                .expression(start, end);
        }
        let kind = match function.as_str() {
            "cast" => Kind::Cast,
            "count" => Kind::Count,
            "sum" => Kind::Sum,
            "min" => Kind::Min,
            "max" => Kind::Max,
            "avg" => Kind::Avg,
            "std" | "stddev" => Kind::Stddev,
            "stddev_pop" => Kind::StddevPop,
            "stddev_samp" => Kind::StddevSamp,
            "variance" => Kind::Variance,
            "var_pop" => Kind::VarPop,
            "var_samp" => Kind::VarSamp,
            "bit_and" => Kind::BitAnd,
            "bit_or" => Kind::BitOr,
            "bit_xor" => Kind::BitXor,
            "approx_count_distinct" => Kind::ApproxCountDistinct,
            "approx_count_distinct_synopsis" => Kind::ApproxCountDistinctSynopsis,
            "approx_count_distinct_synopsis_merge" => Kind::ApproxCountDistinctSynopsisMerge,
            "sum_opnsize" => Kind::SumOpnsize,
            "top_k_fre_hist" => Kind::TopFreHist,
            "hybrid_hist" => Kind::HybridHist,
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
        if matches!(function.as_str(), "substr" | "substring") {
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
        let approximate = matches!(
            kind,
            Kind::ApproxCountDistinct | Kind::ApproxCountDistinctSynopsis
        );
        let histogram = matches!(kind, Kind::TopFreHist | Kind::HybridHist);
        let non_distinct_aggregate = matches!(
            kind,
            Kind::Stddev
                | Kind::StddevPop
                | Kind::StddevSamp
                | Kind::Variance
                | Kind::VarPop
                | Kind::VarSamp
                | Kind::BitAnd
                | Kind::BitOr
                | Kind::BitXor
        );
        let aggregate = approximate
            || histogram
            || non_distinct_aggregate
            || matches!(
                kind,
                Kind::Count
                    | Kind::Sum
                    | Kind::Min
                    | Kind::Max
                    | Kind::Avg
                    | Kind::ApproxCountDistinctSynopsisMerge
                    | Kind::SumOpnsize
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
        let star = arguments.iter().any(|arg| arg.kind == Kind::Star);
        let count_star = kind == Kind::Count
            && !distinct
            && arguments.len() == 1
            && arguments[0].kind == Kind::Star;
        if star && !count_star {
            return Err("only COUNT(*) accepts a wildcard argument");
        }
        if modifier.is_some()
            && (!aggregate
                || approximate
                || histogram
                || distinct && non_distinct_aggregate
                || matches!(
                    kind,
                    Kind::ApproxCountDistinctSynopsisMerge | Kind::SumOpnsize
                ))
        {
            return Err("invalid aggregate modifier");
        }
        let argument_count = match kind {
            Kind::TopFreHist => 4,
            Kind::HybridHist => 2,
            _ => 1,
        };
        if aggregate
            && !count_star
            && (arguments.is_empty()
                || (!(approximate || kind == Kind::Count && distinct)
                    && arguments.len() != argument_count))
        {
            return Err("invalid aggregate argument count");
        }
        if (kind == Kind::TopFreHist
            && (arguments[0].kind != Kind::Number || arguments[2].kind != Kind::Int))
            || (kind == Kind::HybridHist && arguments[1].kind != Kind::Int)
        {
            return Err("histogram rate and bucket count must be numeric constants");
        }
        let children = if ranking || histogram {
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
        } else if approximate {
            vec![Self::new(Kind::ExprList, arguments)]
        } else if kind == Kind::SumOpnsize {
            vec![Self::missing(), arguments.into_iter().next().unwrap()]
        } else if count_star || kind == Kind::ApproxCountDistinctSynopsisMerge {
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

    pub fn projection(mut value: Self, alias: Option<Self>) -> Self {
        let (start, end) = (value.start, value.end);
        if let Some(mut alias) = alias {
            let text = alias.text.clone();
            let string = alias.kind == Kind::String;
            let span = if string {
                (alias.start, alias.end)
            } else {
                (start, end)
            };
            alias.kind = Kind::Ident;
            alias.children.clear();
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
        Self::new(Kind::Projection, vec![value]).at(start, end)
    }

    pub fn select(query: Select) -> Result<Self, &'static str> {
        if query.into.is_some() && query.into_end.is_some() {
            return Err("one INTO clause");
        }
        let projection = Self::list(Kind::ProjectList, query.projection);
        let from = if query.from.is_empty() {
            Self::missing()
        } else {
            Self::new(Kind::FromList, query.from)
        };
        let filter = query
            .filter
            .map(|node| Self::new(Kind::Where, vec![node, Self::missing()]))
            .unwrap_or_else(Self::missing);
        let group = Self::optional(query.group_by);
        let having = query.having.unwrap_or_else(Self::missing);
        let order = if query.order_by.is_empty() {
            Self::missing()
        } else {
            Self::new(Kind::Order, vec![Self::new(Kind::SortList, query.order_by)])
        };
        Self::new(
            Kind::Select,
            vec![
                Self::list(Kind::SelectOptions, query.options),
                projection,
                Self::optional(query.into),
                from,
                filter,
                group,
                having,
                Self::list(Kind::NamedWindows, query.windows),
                Self::optional(query.set),
                order,
                query.limit.unwrap_or_else(Self::missing),
                query.lock.unwrap_or_else(Self::missing),
                Self::list(Kind::Hints, query.hints),
                Self::optional(query.into_end),
                Self::optional(query.approximate),
                Self::optional(query.vector_parameters),
                Self::optional(query.with),
            ],
        )
        .expression(0, 0)
    }
}
