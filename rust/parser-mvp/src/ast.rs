use serde::Serialize;

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
    pub alias: Option<String>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Table {
    pub relation: Relation,
    pub joins: Vec<Join>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Relation {
    pub source: TableSource,
    pub alias: Option<String>,
}

#[derive(Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum TableSource {
    Named { name: Vec<String> },
    Derived { query: Box<Select> },
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Join {
    pub kind: String,
    pub relation: Relation,
    pub condition: Option<Expr>,
    pub using: Vec<String>,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct OrderItem {
    pub expression: Expr,
    pub descending: bool,
}

#[derive(Debug, PartialEq, Serialize)]
pub struct Limit {
    pub count: String,
    pub offset: Option<String>,
}

#[derive(Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Expr {
    Identifier {
        parts: Vec<String>,
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
        qualifier: Vec<String>,
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
        name: Vec<String>,
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
