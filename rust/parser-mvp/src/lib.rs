pub mod ast;

use ast::*;
use pest::iterators::Pair;
use pest_vm::Vm;

pub const GRAMMAR: &str = include_str!("sql.pest");

pub struct Parser {
    vm: Vm,
}

impl Parser {
    /// Construct once and reuse. Matching only borrows the grammar; ASTs own their strings.
    pub fn new() -> Result<Self, String> {
        let (_, rules) = pest_meta::parse_and_optimize(GRAMMAR).map_err(|errors| {
            errors
                .into_iter()
                .map(|e| e.to_string())
                .collect::<Vec<_>>()
                .join("\n")
        })?;
        Ok(Self { vm: Vm::new(rules) })
    }

    pub fn parse(&self, sql: &str) -> Result<Select, String> {
        let query = self
            .vm
            .parse("query", sql)
            .map_err(|e| e.to_string())?
            .next()
            .unwrap();
        Ok(select(children(query).next().unwrap()))
    }

    pub fn parse_expression(&self, sql: &str) -> Result<Expr, String> {
        let input = self
            .vm
            .parse("expression_input", sql)
            .map_err(|e| e.to_string())?
            .next()
            .unwrap();
        Ok(expression(children(input).next().unwrap()))
    }
}

type Part<'a> = Pair<'a, &'a str>;

// Uppercase rules are keywords. Lowercase rules describe semantic structure.
fn children(part: Part<'_>) -> impl Iterator<Item = Part<'_>> {
    part.into_inner()
        .filter(|p| !p.as_rule().as_bytes()[0].is_ascii_uppercase())
}

fn identifier(part: Part<'_>) -> String {
    if part.as_rule() == "quoted_identifier" {
        part.as_str()[1..part.as_str().len() - 1].replace("``", "`")
    } else {
        part.as_str().to_owned()
    }
}

fn name(part: Part<'_>) -> Vec<String> {
    children(part).map(identifier).collect()
}

fn alias(part: Part<'_>) -> String {
    identifier(children(part).next().unwrap())
}

fn select(part: Part<'_>) -> Select {
    let mut query = Select {
        distinct: false,
        projection: Vec::new(),
        from: Vec::new(),
        filter: None,
        group_by: Vec::new(),
        having: None,
        order_by: Vec::new(),
        limit: None,
    };
    for field in children(part) {
        match field.as_rule() {
            "distinct" => query.distinct = true,
            "projection" => {
                query.projection = children(field)
                    .map(|item| {
                        let mut parts = children(item);
                        SelectItem {
                            expression: expression(parts.next().unwrap()),
                            alias: parts.next().map(alias),
                        }
                    })
                    .collect()
            }
            "from_clause" => query.from = children(field).map(table).collect(),
            "where_clause" => query.filter = Some(expression(children(field).next().unwrap())),
            "group_clause" => query.group_by = expression_list(children(field).next().unwrap()),
            "having_clause" => query.having = Some(expression(children(field).next().unwrap())),
            "order_clause" => {
                query.order_by = children(field)
                    .map(|item| {
                        let mut parts = children(item);
                        OrderItem {
                            expression: expression(parts.next().unwrap()),
                            descending: parts
                                .next()
                                .is_some_and(|p| p.as_str().eq_ignore_ascii_case("desc")),
                        }
                    })
                    .collect()
            }
            "limit_clause" => {
                let mut parts = children(field);
                let first = parts.next().unwrap().as_str().to_owned();
                query.limit = Some(match parts.next() {
                    Some(tail) => {
                        let comma = tail.as_rule() == "comma_limit";
                        let second = children(tail).next().unwrap().as_str().to_owned();
                        if comma {
                            Limit {
                                count: second,
                                offset: Some(first),
                            }
                        } else {
                            Limit {
                                count: first,
                                offset: Some(second),
                            }
                        }
                    }
                    None => Limit {
                        count: first,
                        offset: None,
                    },
                });
            }
            rule => unreachable!("unexpected SELECT rule: {rule}"),
        }
    }
    query
}

fn relation(part: Part<'_>) -> Relation {
    let mut parts = children(part);
    let source = parts.next().unwrap();
    let source = if source.as_rule() == "derived_table" {
        TableSource::Derived {
            query: Box::new(select(children(source).next().unwrap())),
        }
    } else {
        TableSource::Named { name: name(source) }
    };
    Relation {
        source,
        alias: parts.next().map(alias),
    }
}

fn table(part: Part<'_>) -> Table {
    let mut parts = children(part);
    Table {
        relation: relation(parts.next().unwrap()),
        joins: parts.map(join).collect(),
    }
}

fn join(part: Part<'_>) -> Join {
    let mut parts = children(part).peekable();
    let kind = if matches!(
        parts.peek().unwrap().as_rule(),
        "join_kind" | "outer_join_kind"
    ) {
        // Read the keyword token, so comments between LEFT and OUTER do not enter the AST.
        parts
            .next()
            .unwrap()
            .into_inner()
            .next()
            .unwrap()
            .as_str()
            .to_ascii_lowercase()
    } else {
        "inner".to_owned()
    };
    let mut result = Join {
        kind,
        relation: relation(parts.next().unwrap()),
        condition: None,
        using: Vec::new(),
    };
    if let Some(condition) = parts.next() {
        for field in children(condition) {
            if field.as_rule() == "expression" {
                result.condition = Some(expression(field));
            } else {
                result.using.push(identifier(field));
            }
        }
    }
    result
}

fn binary(op: String, left: Expr, right: Expr) -> Expr {
    Expr::Binary {
        op,
        left: Box::new(left),
        right: Box::new(right),
    }
}

fn expression_list(part: Part<'_>) -> Vec<Expr> {
    children(part).map(expression).collect()
}

fn expression(part: Part<'_>) -> Expr {
    match part.as_rule() {
        "expression" => expression(children(part).next().unwrap()),
        "or_expr" | "xor_expr" | "and_expr" | "bit_or_expr" | "bit_and_expr" | "shift_expr"
        | "sum_expr" | "product_expr" | "bit_xor_expr" => {
            let mut parts = children(part);
            let mut result = expression(parts.next().unwrap());
            while let Some(op) = parts.next() {
                result = binary(
                    op.as_str().trim().to_ascii_lowercase(),
                    result,
                    expression(parts.next().unwrap()),
                );
            }
            result
        }
        "not_expr" | "unary_expr" => {
            let mut parts: Vec<_> = children(part).collect();
            let mut result = expression(parts.pop().unwrap());
            for op in parts.into_iter().rev() {
                result = Expr::Unary {
                    op: op.as_str().trim().to_ascii_lowercase(),
                    value: Box::new(result),
                };
            }
            result
        }
        "predicate" => {
            let mut parts = children(part);
            let mut result = expression(parts.next().unwrap());
            for suffix in parts {
                let rule = suffix.as_rule();
                let mut tail = children(suffix).peekable();
                let negated = tail.peek().is_some_and(|p| p.as_rule() == "negated");
                if negated {
                    tail.next();
                }
                result = match rule {
                    "comparison" => binary(
                        tail.next().unwrap().as_str().to_owned(),
                        result,
                        expression(tail.next().unwrap()),
                    ),
                    "between" => Expr::Between {
                        value: Box::new(result),
                        low: Box::new(expression(tail.next().unwrap())),
                        high: Box::new(expression(tail.next().unwrap())),
                        negated,
                    },
                    "in_list" => {
                        let body = tail.next().unwrap();
                        let (values, query) = if body.as_rule() == "select" {
                            (Vec::new(), Some(Box::new(select(body))))
                        } else {
                            (expression_list(body), None)
                        };
                        Expr::In {
                            value: Box::new(result),
                            values,
                            query,
                            negated,
                        }
                    }
                    "like" => binary(
                        if negated { "not like" } else { "like" }.to_owned(),
                        result,
                        expression(tail.next().unwrap()),
                    ),
                    "is_test" => Expr::Is {
                        value: Box::new(result),
                        test: Box::new(expression(tail.next().unwrap())),
                        negated,
                    },
                    _ => unreachable!("unexpected predicate: {rule}"),
                };
            }
            result
        }
        "nested" => {
            let inner = children(part).next().unwrap();
            if inner.as_rule() == "select" {
                Expr::Subquery {
                    query: Box::new(select(inner)),
                }
            } else {
                expression(inner)
            }
        }
        "exists" => Expr::Exists {
            query: Box::new(select(children(part).next().unwrap())),
        },
        "function" => {
            let mut parts = children(part).peekable();
            let name = name(parts.next().unwrap());
            let distinct = parts.peek().is_some_and(|p| p.as_rule() == "distinct");
            if distinct {
                parts.next();
            }
            Expr::Call {
                name,
                distinct,
                arguments: parts.next().map(expression_list).unwrap_or_default(),
            }
        }
        "qualified_name" => Expr::Identifier { parts: name(part) },
        "qualified_wildcard" => Expr::Wildcard {
            qualifier: name(children(part).next().unwrap()),
        },
        "wildcard" => Expr::Wildcard {
            qualifier: Vec::new(),
        },
        "parameter" => Expr::Parameter,
        "null" => Expr::Null,
        "boolean" => Expr::Boolean {
            value: part.as_str().eq_ignore_ascii_case("true"),
        },
        "number" => Expr::Number {
            text: part.as_str().to_owned(),
        },
        "string" => Expr::String {
            value: decode_string(part.as_str()),
        },
        rule => unreachable!("unexpected expression: {rule}"),
    }
}

fn decode_string(text: &str) -> String {
    let mut chars = text[1..text.len() - 1].chars();
    let mut result = String::new();
    while let Some(ch) = chars.next() {
        match ch {
            '\'' => {
                chars.next();
                result.push('\'');
            }
            '\\' => result.push(match chars.next().unwrap() {
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                '0' => '\0',
                ch => ch,
            }),
            ch => result.push(ch),
        }
    }
    result
}
