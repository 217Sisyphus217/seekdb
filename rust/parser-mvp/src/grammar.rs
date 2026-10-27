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

use crate::{ast::*, SqlMode};

fn reserved(word: &str) -> bool {
    [
        "select", "distinct", "as", "from", "where", "group", "by", "having", "order", "limit",
        "offset", "asc", "desc", "join", "inner", "left", "right", "outer", "cross", "on", "using",
        "or", "xor", "and", "not", "between", "in", "like", "is", "null", "true", "false",
        "exists", "div", "mod", "union",
    ]
    .iter()
    .any(|keyword| word.eq_ignore_ascii_case(keyword))
}

fn decode(text: &str, no_backslash_escapes: bool) -> String {
    let quote = text.chars().next().unwrap();
    let mut chars = text[1..text.len() - 1].chars();
    let mut result = String::new();
    while let Some(ch) = chars.next() {
        if ch == quote {
            chars.next();
            result.push(ch);
        } else if ch == '\\' && !no_backslash_escapes {
            match chars.next().unwrap() {
                '0' => result.push('\0'),
                'b' => result.push('\x08'),
                'n' => result.push('\n'),
                'r' => result.push('\r'),
                't' => result.push('\t'),
                'Z' => result.push('\x1a'),
                ch @ ('%' | '_') => {
                    result.push('\\');
                    result.push(ch);
                }
                ch => result.push(ch),
            }
        } else {
            result.push(ch);
        }
    }
    result
}

fn binary(op: String, left: Expr, right: Expr) -> Result<Expr, &'static str> {
    let (start, end) = (left.start, right.end);
    Expr::new(
        ExprKind::Binary {
            op,
            left: Box::new(left),
            right: Box::new(right),
        },
        start,
        end,
    )
}

fn chain(first: Expr, rest: Vec<(String, Expr)>) -> Result<Expr, &'static str> {
    rest.into_iter()
        .try_fold(first, |left, (op, right)| binary(op, left, right))
}

enum Tail {
    Compare(String, Expr),
    Between(bool, Expr, Expr),
    In(bool, Vec<Expr>, Option<Box<Select>>, usize),
    Is(bool, Expr),
}

peg::parser! {
    pub grammar sql(mode: SqlMode) for str {
        rule space() = [' ' | '\t' | '\r' | '\n' | '\x0b' | '\x0c']
        rule _() = quiet!{(space() / "/*" !['!' | '+'] (!"*/" [_])* "*/"
            / ("--" &(space() / ![_]) / "#") (!"\n" [_])*)*}
        rule cont() = ['a'..='z' | 'A'..='Z' | '0'..='9' | '_' | '$' | '\u{80}'..='\u{10ffff}']
        rule kw(word: &'static str) -> ()
            = text:$([_] *<{word.len()}>) !cont() _ {? if text.eq_ignore_ascii_case(word) { Ok(()) } else { Err(word) } }
        rule depth(level: usize)
            = {? if level < MAX_DEPTH { Ok(()) } else { Err("SQL nesting below 128 levels") } }
        rule enabled(value: bool) = {? if value { Ok(()) } else { Err("SQL mode") } }
        rule ident() -> Name
            = start:position!() text:$("`" ("``" / !"`" [_])+ "`") end:position!() _
                { Name { text: text[1..text.len()-1].replace("``", "`"), start, end } }
            / enabled(mode.ansi_quotes) start:position!() text:$("\"" ("\"\"" / !"\"" [_])+ "\"") end:position!() _
                { Name { text: text[1..text.len()-1].replace("\"\"", "\""), start, end } }
            / start:position!() text:$(['a'..='z' | 'A'..='Z' | '_' | '$' | '\u{80}'..='\u{10ffff}'] cont()*) end:position!() _
                {? if reserved(text) { Err("identifier") } else { Ok(Name { text: text.to_owned(), start, end }) } }
        rule name() -> Vec<Name> = parts:(ident() ++ ("." _)) { parts }
        rule alias() -> Name = kw("as")? value:ident() { value }
        rule number() -> Expr
            = start:position!() text:$((['0'..='9']+ ("." ['0'..='9']*)? / "." ['0'..='9']+)
                (['e' | 'E'] ['+' | '-']? ['0'..='9']+)?) !cont() end:position!() _
                {? Expr::new(ExprKind::Number { text: text.to_owned() }, start, end) }
        rule string() -> Expr
            = start:position!() text:$("'" ("''" / enabled(!mode.no_backslash_escapes) "\\" [_]
                / !("'" / enabled(!mode.no_backslash_escapes) "\\") [_])* "'") end:position!() _
                {? Expr::new(ExprKind::String { value: decode(text, mode.no_backslash_escapes) }, start, end) }
            / enabled(!mode.ansi_quotes) start:position!() text:$("\"" ("\"\"" / enabled(!mode.no_backslash_escapes) "\\" [_]
                / !("\"" / enabled(!mode.no_backslash_escapes) "\\") [_])* "\"") end:position!() _
                {? Expr::new(ExprKind::String { value: decode(text, mode.no_backslash_escapes) }, start, end) }
        rule bool_null() -> Expr
            = start:position!() kw("null") {? Expr::new(ExprKind::Null, start, start+4) }
            / start:position!() kw("true") {? Expr::new(ExprKind::Boolean { value: true }, start, start+4) }
            / start:position!() kw("false") {? Expr::new(ExprKind::Boolean { value: false }, start, start+5) }
        rule parameter() -> Expr
            = start:position!() "?" end:position!() _ {? Expr::new(ExprKind::Parameter, start, end) }
        rule atom(level: usize) -> Expr
            = depth(level) start:position!() "(" _ query:select(level+1) ")" end:position!() _
                {? Expr::new(ExprKind::Subquery { query: Box::new(query) }, start, end) }
            / depth(level) start:position!() "(" _ value:expr(level+1) ")" end:position!() _
                {? Expr::new(ExprKind::Parenthesized { value: Box::new(value) }, start, end) }
            / depth(level) start:position!() kw("exists") "(" _ query:select(level+1) ")" end:position!() _
                {? Expr::new(ExprKind::Exists { query: Box::new(query) }, start, end) }
            / start:position!() "*" end:position!() _ {? Expr::new(ExprKind::Wildcard { qualifier: vec![] }, start, end) }
            / number() / string() / bool_null() / parameter()
            / depth(level) start:position!() parts:name() tail:("." _ "*" end:position!() _ { (None, end) }
                / "(" _ distinct:kw("distinct")? args:expr_list(level+1)? ")" end:position!() _
                    { (Some((distinct.is_some(), args.unwrap_or_default())), end) })?
                {? let (kind, end) = match tail {
                    None => { let end=parts.last().unwrap().end; (ExprKind::Identifier { parts }, end) },
                    Some((None, end)) => (ExprKind::Wildcard { qualifier: parts }, end),
                    Some((Some((distinct, arguments)), end)) => (ExprKind::Call { name: parts, distinct, arguments }, end),
                }; Expr::new(kind, start, end) }
        rule unary(level: usize) -> Expr
            = ops:(start:position!() op:$(['+' | '-' | '!' | '~']) _ { (start, op) }
                / enabled(mode.high_not_precedence) start:position!() kw("not") { (start, "not") })* value:atom(level)
                {? ops.into_iter().rev().try_fold(value, |value, (start, op)| {
                    let end=value.end; Expr::new(ExprKind::Unary { op: op.into(), value: Box::new(value) }, start, end)
                }) }
        rule concat(level: usize) -> Expr = left:unary(level) rest:(enabled(mode.pipes_as_concat) "||" _ right:unary(level) { ("concat".into(), right) })* {? chain(left, rest) }
        rule bit_xor(level: usize) -> Expr = left:concat(level) rest:("^" _ right:concat(level) { ("^".into(), right) })* {? chain(left, rest) }
        rule product(level: usize) -> Expr = left:bit_xor(level) rest:(op:(s:$("*" / "/" / "%") _ { s } / kw("div") { "div" } / kw("mod") { "mod" }) right:bit_xor(level) { (op.into(), right) })* {? chain(left, rest) }
        rule sum(level: usize) -> Expr = left:product(level) rest:(op:$("+" / "-") _ right:product(level) { (op.into(), right) })* {? chain(left, rest) }
        rule shift(level: usize) -> Expr = left:sum(level) rest:(op:$("<<" / ">>") _ right:sum(level) { (op.into(), right) })* {? chain(left, rest) }
        rule bit_and(level: usize) -> Expr = left:shift(level) rest:("&" !"&" _ right:shift(level) { ("&".into(), right) })* {? chain(left, rest) }
        rule bit_or(level: usize) -> Expr = left:bit_and(level) rest:("|" !"|" _ right:bit_and(level) { ("|".into(), right) })* {? chain(left, rest) }
        rule tail(level: usize) -> Tail
            = op:$("<=>" / "<=" / ">=" / "<>" / "!=" / "=" / "<" / ">") _ right:bit_or(level) { Tail::Compare(op.into(), right) }
            / kw("is") negated:kw("not")? test:bool_null() { Tail::Is(negated.is_some(), test) }
            / negated:kw("not")? tail:(kw("between") low:bit_or(level) kw("and") high:bit_or(level) { Tail::Between(false, low, high) }
                / kw("like") right:bit_or(level) { Tail::Compare("like".into(), right) }
                / kw("in") "(" _ values:(query:select(level+1) { (vec![], Some(Box::new(query))) } / values:expr_list(level+1) { (values, None) }) ")" end:position!() _ { Tail::In(false, values.0, values.1, end) })
                { match tail {
                    Tail::Between(_, low, high) => Tail::Between(negated.is_some(), low, high),
                    Tail::In(_, values, query, end) => Tail::In(negated.is_some(), values, query, end),
                    Tail::Compare(_, right) => Tail::Compare(if negated.is_some() { "not like" } else { "like" }.into(), right),
                    _ => unreachable!(),
                } }
        rule predicate(level: usize) -> Expr = left:bit_or(level) tails:tail(level)* {?
            tails.into_iter().try_fold(left, |value, tail| {
                let start=value.start;
                match tail {
                    Tail::Compare(op, right) => binary(op, value, right),
                    Tail::Between(negated, low, high) => { let end=high.end; Expr::new(ExprKind::Between { value: Box::new(value), low: Box::new(low), high: Box::new(high), negated }, start, end) },
                    Tail::In(negated, values, query, end) => Expr::new(ExprKind::In { value: Box::new(value), values, query, negated }, start, end),
                    Tail::Is(negated, test) => { let end=test.end; Expr::new(ExprKind::Is { value: Box::new(value), test: Box::new(test), negated }, start, end) },
                }
            })
        }
        rule not_expr(level: usize) -> Expr = ops:(enabled(!mode.high_not_precedence) start:position!() kw("not") { start })* value:predicate(level) {?
            ops.into_iter().rev().try_fold(value, |value, start| { let end=value.end; Expr::new(ExprKind::Unary { op: "not".into(), value: Box::new(value) }, start, end) })
        }
        rule and_expr(level: usize) -> Expr = left:not_expr(level) rest:((kw("and") / "&&" _) right:not_expr(level) { ("and".into(), right) })* {? chain(left, rest) }
        rule xor_expr(level: usize) -> Expr = left:and_expr(level) rest:(kw("xor") right:and_expr(level) { ("xor".into(), right) })* {? chain(left, rest) }
        rule expr(level: usize) -> Expr = left:xor_expr(level) rest:((kw("or") / enabled(!mode.pipes_as_concat) "||" _) right:xor_expr(level) { ("or".into(), right) })* {? chain(left, rest) }
        rule expr_list(level: usize) -> Vec<Expr> = values:(expr(level) ++ ("," _)) { values }
        rule item(level: usize) -> SelectItem = expression:expr(level) alias:alias()? { SelectItem { expression, alias } }
        rule relation(level: usize) -> Relation = source:("(" _ query:select(level+1) ")" _ { TableSource::Derived { query: Box::new(query) } } / name:name() { TableSource::Named { name } }) alias:alias()? { Relation { source, alias } }
        rule join(level: usize) -> Join = kind:((kw("left") kw("outer")?) { "left" } / (kw("right") kw("outer")?) { "right" } / kw("inner") { "inner" } / kw("cross") { "cross" })? kw("join") relation:relation(level)
            condition:(kw("on") value:expr(level) { (Some(value), vec![]) } / kw("using") "(" _ names:(ident() ++ ("," _)) ")" _ { (None, names) })? {?
                let kind=kind.unwrap_or("inner");
                if (kind=="left" || kind=="right") && condition.is_none() { Err("join condition") }
                else { let (condition, using)=condition.unwrap_or_default(); Ok(Join { kind: kind.into(), relation, condition, using }) }
            }
        rule table(level: usize) -> Table = relation:relation(level) joins:join(level)* { Table { relation, joins } }
        rule order_item(level: usize) -> OrderItem = expression:expr(level) direction:(kw("asc") { false } / kw("desc") { true })? { OrderItem { expression, descending: direction.unwrap_or(false) } }
        rule limit_value() -> Expr = parameter() / start:position!() text:$(['0'..='9']+) !cont() end:position!() _ {? Expr::new(ExprKind::Number { text: text.into() }, start, end) }
        rule limit() -> Limit = kw("limit") first:limit_value() tail:(kw("offset") value:limit_value() { (false, value) } / "," _ value:limit_value() { (true, value) })? {
            match tail {
                Some((true, count)) => Limit { count, offset: Some(first), comma: true },
                Some((false, offset)) => Limit { count: first, offset: Some(offset), comma: false },
                None => Limit { count: first, offset: None, comma: false },
            }
        }
        rule select(level: usize) -> Select = depth(level) kw("select") ("/*+parser_mvp*/" _)? distinct:kw("distinct")? projection:(item(level) ++ ("," _))
            from:(kw("from") tables:(table(level) ++ ("," _)) { tables })?
            filter:(kw("where") value:expr(level) { value })?
            group_by:(kw("group") kw("by") values:expr_list(level) { values })?
            having:(kw("having") value:expr(level) { value })?
            order_by:(kw("order") kw("by") values:(order_item(level) ++ ("," _)) { values })?
            limit:limit()? { Select { distinct: distinct.is_some(), projection, from: from.unwrap_or_default(), filter, group_by: group_by.unwrap_or_default(), having, order_by: order_by.unwrap_or_default(), limit } }
        pub rule query() -> Select = _ value:select(0) (";" _)? { value }
        pub rule expression_input() -> Expr = _ value:expr(0) { value }
    }
}
