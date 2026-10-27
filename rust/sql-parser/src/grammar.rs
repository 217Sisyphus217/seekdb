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

use crate::{node::*, SqlMode};

fn reserved(word: &str) -> bool {
    [
        "select",
        "distinct",
        "as",
        "from",
        "where",
        "group",
        "by",
        "having",
        "order",
        "limit",
        "offset",
        "asc",
        "desc",
        "join",
        "inner",
        "left",
        "right",
        "outer",
        "cross",
        "on",
        "using",
        "or",
        "xor",
        "and",
        "not",
        "between",
        "in",
        "like",
        "is",
        "null",
        "true",
        "false",
        "exists",
        "div",
        "mod",
        "union",
        "insert",
        "replace",
        "update",
        "delete",
        "set",
        "into",
        "values",
        "default",
        "create",
        "drop",
        "table",
        "database",
        "schema",
        "primary",
        "key",
        "unique",
        "constraint",
        "references",
        "commit",
        "rollback",
        "case",
        "when",
        "then",
        "else",
        "end",
        "for",
        "lock",
        "cast",
        "convert",
        "lateral",
        "over",
        "partition",
        "rows",
        "range",
        "window",
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

fn chain(first: Node, rest: Vec<(String, Node)>) -> Result<Node, &'static str> {
    rest.into_iter()
        .try_fold(first, |left, (op, right)| Node::binary(op, left, right))
}

enum Tail {
    Compare(String, Node),
    Between(bool, Node, Node),
    In(bool, Node, usize),
    Is(bool, Node),
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
                { Name { text: text[1..text.len()-1].replace("``", "`"), quoted: true, start, end } }
            / enabled(mode.ansi_quotes) start:position!() text:$("\"" ("\"\"" / !"\"" [_])+ "\"") end:position!() _
                { Name { text: text[1..text.len()-1].replace("\"\"", "\""), quoted: true, start, end } }
            / start:position!() text:$(['a'..='z' | 'A'..='Z' | '_' | '$' | '\u{80}'..='\u{10ffff}'] cont()*) end:position!() _
                {? if reserved(text) { Err("identifier") } else { Ok(Name { text: text.to_owned(), quoted: false, start, end }) } }
        rule name() -> Vec<Name> = parts:(ident() ++ ("." _)) { parts }
        rule expression_name() -> Vec<Name>
            = name()
            / start:position!() name:(kw("database") { "database" } / kw("schema") { "schema" }
                / kw("replace") { "replace" } / kw("insert") { "insert" } / kw("values") { "values" }
                / kw("left") { "left" } / kw("right") { "right" } / kw("mod") { "mod" }) &"("
                { vec![Name { text: name.into(), quoted: false, start, end: start + name.len() }] }
        rule alias() -> Name = kw("as")? value:ident() { value }
        rule number() -> Node
            = start:position!() text:$((['0'..='9']+ ("." ['0'..='9']*)? / "." ['0'..='9']+)
                (['e' | 'E'] ['+' | '-']? ['0'..='9']+)?) !cont() end:position!() _
                {? Node::number(text, start, end) }
        rule string() -> Node
            = start:position!() text:$("'" ("''" / enabled(!mode.no_backslash_escapes) "\\" [_]
                / !("'" / enabled(!mode.no_backslash_escapes) "\\") [_])* "'") end:position!() _
                {? Node::new(Kind::String, vec![]).text(decode(text, mode.no_backslash_escapes)).expression(start, end) }
            / enabled(!mode.ansi_quotes) start:position!() text:$("\"" ("\"\"" / enabled(!mode.no_backslash_escapes) "\\" [_]
                / !("\"" / enabled(!mode.no_backslash_escapes) "\\") [_])* "\"") end:position!() _
                {? Node::new(Kind::String, vec![]).text(decode(text, mode.no_backslash_escapes)).expression(start, end) }
        rule bool_null() -> Node
            = start:position!() kw("null") {? Node::new(Kind::Null, vec![]).expression(start, start+4) }
            / start:position!() kw("true") {? Node::new(Kind::Bool, vec![]).value(1).expression(start, start+4) }
            / start:position!() kw("false") {? Node::new(Kind::Bool, vec![]).value(0).expression(start, start+5) }
        rule parameter() -> Node
            = start:position!() "?" end:position!() _ {? Node::new(Kind::Parameter, vec![]).expression(start, end) }
        rule hex_digit() = ['0'..='9' | 'a'..='f' | 'A'..='F']
        rule binary_literal() -> Node
            = start:position!() ['x'|'X'] "'" text:$(hex_digit()*) "'" end:position!() _ {?
                if text.len()%2!=0 { Err("even number of hexadecimal digits") }
                else { Node::new(Kind::HexString,vec![]).text(text.into()).expression(start,end) }
            }
            / start:position!() "0x" text:$(hex_digit()+) !cont() end:position!() _ {?
                Node::new(Kind::HexString,vec![]).text(text.into()).expression(start,end)
            }
            / start:position!() text:(['b'|'B'] "'" text:$(['0'|'1']*) "'" { text }
                / "0b" text:$(['0'|'1']+) !cont() { text }) end:position!() _ {?
                Node::new(Kind::BitString,vec![]).text(text.into()).expression(start,end)
            }
        rule scope() -> u64 = kw("global") { 1 } / (kw("session") / kw("local")) { 2 }
        rule system_variable() -> Node = start:position!() "@@" scope:(value:scope() "." _ { value })? name:ident() {
            let end=name.end;
            Node::new(Kind::SystemVariable, vec![]).text(name.text).value(scope.unwrap_or(0)).at(start,end)
        }
        rule user_variable() -> Node = start:position!() "@" !"@" name:(string()
            / start:position!() text:$((cont() / ".")+) end:position!() _ { Node::new(Kind::Ident,vec![]).text(text.into()).at(start,end) }
            / name:ident() { Node::name(name) }) {
            let end=name.end;
            Node::new(Kind::UserVariable, vec![]).text(name.text).at(start,end)
        }
        rule atom(level: usize) -> Node
            = depth(level) start:position!() value:(kw("cast") "(" _ value:expr(level+1) kw("as") target:cast_type() { (value,target) }
                / kw("convert") "(" _ value:expr(level+1) "," _ target:cast_type() { (value,target) }) ")" end:position!() _ {?
                Node::call(vec![Name { text:"cast".into(),quoted:false,start,end:start+4 }],false,vec![value.0,value.1],None,start,end)
            }
            / depth(level) start:position!() kw("case") operand:expr(level+1)?
                branches:(kw("when") condition:expr(level+1) kw("then") value:expr(level+1)
                    { Node::new(Kind::When, vec![condition,value]) })+
                default:(kw("else") value:expr(level+1) { value })? end:position!() kw("end") {?
                    Node::new(Kind::Case, vec![Node::optional(operand),Node::new(Kind::WhenList,branches),
                        default.unwrap_or_else(|| Node::new(Kind::DefaultNull,vec![]))]).expression(start,end+3)
                }
            / system_variable()
            / value:user_variable() {? let (start,end)=(value.start,value.end); Node::new(Kind::GetUserVariable,vec![value]).expression(start,end) }
            / depth(level) start:position!() "(" _ query:select(level+1) ")" end:position!() _
                {? query.expression(start, end) }
            / depth(level) start:position!() "(" _ first:expr(level+1) rest:("," _ values:expr_list(level+1) { values })? ")" end:position!() _ {?
                match rest {
                    None => Node::new(Kind::Parenthesized,vec![first]).expression(start,end),
                    Some(rest) => { let mut values=vec![first]; values.extend(rest); Node::new(Kind::ExprList,values).expression(start,end) }
                }
            }
            / depth(level) start:position!() kw("row") "(" _ first:expr(level+1) "," _ rest:expr_list(level+1) ")" end:position!() _ {?
                let mut values=vec![first]; values.extend(rest); Node::new(Kind::ExprList,values).expression(start,end)
            }
            / depth(level) start:position!() kw("exists") "(" _ query:select(level+1) ")" end:position!() _
                {? Node::new(Kind::Exists, vec![query]).expression(start, end) }
            / start:position!() "*" end:position!() _ {? Node::new(Kind::Star, vec![]).expression(start, end) }
            / binary_literal() / number() / string() / bool_null() / parameter()
            / depth(level) start:position!() !(['x'|'X'] "'" hex_digit()* "'") parts:expression_name() tail:("." _ "*" end:position!() _ { (None, end) }
                / "(" _ distinct:kw("distinct")? args:expr_list(level+1)? ")" close:position!() _
                    window:(kw("over") value:window(level+1) { value })?
                    { let end=window.as_ref().map_or(close,|value| value.end); (Some((distinct.is_some(), args.unwrap_or_default(),window)), end) })?
                {? match tail {
                    None => {
                        let end=parts.last().unwrap().end;
                        let keyword_function=parts.len()==1 && !parts[0].quoted && ["current_timestamp","localtime","localtimestamp",
                            "current_date","current_time","current_user","utc_timestamp","utc_date","utc_time"]
                            .iter().any(|name| parts[0].text.eq_ignore_ascii_case(name));
                        if keyword_function { Node::call(parts,false,vec![],None,start,end) }
                        else { Node::column(parts,false,start,end) }
                    },
                    Some((None, end)) => Node::column(parts, true, start, end),
                    Some((Some((distinct, arguments, window)), end)) => Node::call(parts, distinct, arguments, window, start, end),
                } }
        rule unary(level: usize) -> Node
            = ops:(start:position!() op:$(['+' | '-' | '!' | '~']) _ { (start, op) }
                / enabled(mode.high_not_precedence) start:position!() kw("not") { (start, "not") }
                / start:position!() kw("binary") { (start, "binary") })* value:atom(level)
                {? ops.into_iter().rev().try_fold(value, |value, (start, op)| {
                    Node::unary(op, value, start)
                }) }
        rule concat(level: usize) -> Node = left:unary(level) rest:(enabled(mode.pipes_as_concat) "||" _ right:unary(level) { ("concat".into(), right) })* {? chain(left, rest) }
        rule bit_xor(level: usize) -> Node = left:concat(level) rest:("^" _ right:concat(level) { ("^".into(), right) })* {? chain(left, rest) }
        rule product(level: usize) -> Node = left:bit_xor(level) rest:(op:(s:$("*" / "/" / "%") _ { s } / kw("div") { "div" } / kw("mod") { "mod" }) right:bit_xor(level) { (op.into(), right) })* {? chain(left, rest) }
        rule sum(level: usize) -> Node = left:product(level) rest:(op:$("+" / "-") _ right:product(level) { (op.into(), right) })* {? chain(left, rest) }
        rule shift(level: usize) -> Node = left:sum(level) rest:(op:$("<<" / ">>") _ right:sum(level) { (op.into(), right) })* {? chain(left, rest) }
        rule bit_and(level: usize) -> Node = left:shift(level) rest:("&" !"&" _ right:shift(level) { ("&".into(), right) })* {? chain(left, rest) }
        rule bit_or(level: usize) -> Node = left:bit_and(level) rest:("|" !"|" _ right:bit_and(level) { ("|".into(), right) })* {? chain(left, rest) }
        rule tail(level: usize) -> Tail
            = op:$("<=>" / "<=" / ">=" / "<>" / "!=" / "=" / "<" / ">") _ right:bit_or(level) { Tail::Compare(op.into(), right) }
            / kw("is") negated:kw("not")? test:bool_null() { Tail::Is(negated.is_some(), test) }
            / negated:kw("not")? tail:(kw("between") low:bit_or(level) kw("and") high:bit_or(level) { Tail::Between(false, low, high) }
                / kw("like") right:bit_or(level) { Tail::Compare("like".into(), right) }
                / kw("in") "(" _ values:(select(level+1) / values:expr_list(level+1) { Node::new(Kind::ExprList, values) }) ")" end:position!() _ { Tail::In(false, values, end) })
                { match tail {
                    Tail::Between(_, low, high) => Tail::Between(negated.is_some(), low, high),
                    Tail::In(_, values, end) => Tail::In(negated.is_some(), values, end),
                    Tail::Compare(_, right) => Tail::Compare(if negated.is_some() { "not like" } else { "like" }.into(), right),
                    _ => unreachable!(),
                } }
        rule predicate(level: usize) -> Node = left:bit_or(level) tails:tail(level)* {?
            tails.into_iter().try_fold(left, |value, tail| {
                let start=value.start;
                match tail {
                    Tail::Compare(op, right) => Node::binary(op, value, right),
                    Tail::Between(negated, low, high) => { let end=high.end; Node::new(Kind::Between, vec![value, low, high]).value(u64::from(negated)).expression(start, end) },
                    Tail::In(negated, right, end) => Node::new(Kind::In, vec![value, right]).value(u64::from(negated)).expression(start, end),
                    Tail::Is(negated, test) => { let end=test.end; Node::new(Kind::Is, vec![value, test]).value(u64::from(negated)).expression(start, end) },
                }
            })
        }
        rule not_expr(level: usize) -> Node = ops:(enabled(!mode.high_not_precedence) start:position!() kw("not") { start })* value:predicate(level) {?
            ops.into_iter().rev().try_fold(value, |value, start| { Node::unary("not", value, start) })
        }
        rule and_expr(level: usize) -> Node = left:not_expr(level) rest:((kw("and") / "&&" _) right:not_expr(level) { ("and".into(), right) })* {? chain(left, rest) }
        rule xor_expr(level: usize) -> Node = left:and_expr(level) rest:(kw("xor") right:and_expr(level) { ("xor".into(), right) })* {? chain(left, rest) }
        rule expr(level: usize) -> Node = left:xor_expr(level) rest:((kw("or") / enabled(!mode.pipes_as_concat) "||" _) right:xor_expr(level) { ("or".into(), right) })* {? chain(left, rest) }
        rule expr_list(level: usize) -> Vec<Node> = values:(expr(level) ++ ("," _)) { values }
        rule item(level: usize) -> Node = expression:expr(level) alias:(kw("as")? value:(name:ident() { Node::name(name) } / string()) { value })? {? Node::projection(expression, alias) }
        rule relation(level: usize) -> Node = lateral:kw("lateral")? source:("(" _ query:select(level+1) ")" _ { query }
            / name:name() {? Node::relation(name) }) alias:alias()?
            columns:("(" _ names:(ident() ++ ("," _)) ")" _ { names })? {?
                if (lateral.is_some() || columns.is_some()) && source.kind!=Kind::Select { return Err("derived table"); }
                let mut table=Node::table(source,alias)?;
                if let Some(columns)=columns {
                    let alias=std::mem::replace(&mut table.children[1],Node::missing());
                    table.children[1]=Node::new(Kind::Link,vec![alias,Node::new(Kind::ColumnList,columns.into_iter().map(Node::name).collect())]);
                }
                table.value=u64::from(lateral.is_some());
                Ok(table)
            }
        rule join(level: usize) -> (Kind, Node, Node) = kind:((kw("left") kw("outer")?) { Kind::LeftJoin }
            / (kw("right") kw("outer")?) { Kind::RightJoin } / kw("inner") { Kind::InnerJoin } / kw("cross") { Kind::InnerJoin })?
            kw("join") relation:relation(level)
            condition:(kw("on") value:expr(level) { value }
                / kw("using") "(" _ names:(ident() ++ ("," _)) ")" _ { Node::new(Kind::ColumnList, names.into_iter().map(Node::name).collect()) })? {?
                let kind=kind.unwrap_or(Kind::InnerJoin);
                if matches!(kind, Kind::LeftJoin | Kind::RightJoin) && condition.is_none() { Err("join condition") }
                else { Ok((kind, relation, condition.unwrap_or_else(Node::missing))) }
            }
        rule table(level: usize) -> Node = relation:relation(level) joins:join(level)* {?
            joins.into_iter().try_fold(relation, |left, (kind, right, condition)| {
                Node::new(Kind::JoinedTable, vec![Node::new(kind, vec![]), left, right, condition, Node::missing()]).expression(0, 0)
            })
        }
        rule order_item(level: usize) -> Node = expression:expr(level) direction:(kw("asc") { Kind::Asc } / kw("desc") { Kind::Desc })? {
            Node::new(Kind::SortKey, vec![expression, Node::new(direction.unwrap_or(Kind::Asc), vec![])])
        }
        rule window_bound() -> Node
            = kw("current") kw("row") { Node::new(Kind::WindowBound,vec![]).value(1) }
            / value:(start:position!() kw("unbounded") {
                    Node::new(Kind::WindowInterval,vec![Node::new(Kind::Ident,vec![]).text("unbounded".into())]).text("unbounded".into()).value(1).at(start,start+9)
                }
                / value:(number() / parameter()) {
                    let (start,end)=(value.start,value.end);
                    Node::new(Kind::WindowInterval,vec![value]).value(1).at(start,end)
                }) direction:(kw("preceding") { 1 } / kw("following") { 2 }) {
                    Node::new(Kind::WindowBound,vec![value,Node::new(Kind::HiddenInt,vec![]).value(direction)]).value(2)
                }
        rule window_frame() -> Node = kind:(kw("rows") { 1 } / kw("range") { 2 })
            bounds:(kw("between") first:window_bound() kw("and") last:window_bound() { (1,vec![first,last]) }
                / first:window_bound() { (2,vec![first]) }) {
                let mut children=vec![Node::new(Kind::HiddenInt,vec![]).value(kind)];
                children.extend(bounds.1);
                Node::new(Kind::WindowFrame,children).value(bounds.0)
            }
        rule window_spec(level: usize) -> Node = start:position!() "(" _ name:ident()?
            partition:(kw("partition") kw("by") values:expr_list(level) { values })?
            order:(kw("order") kw("by") values:(order_item(level) ++ ("," _)) { values })?
            frame:window_frame()? ")" end:position!() _ {
                Node::new(Kind::Window,vec![Node::optional(name.map(Node::name)),
                    Node::new(Kind::WindowSpec,vec![Node::list(Kind::ExprList,partition.unwrap_or_default()),
                        Node::order_clause(order.unwrap_or_default()),Node::optional(frame)])]).at(start,end)
            }
        rule window(level: usize) -> Node = window_spec(level) / name:ident() {
            let (start,end)=(name.start,name.end);
            Node::new(Kind::Window,vec![Node::name(name),Node::missing()]).at(start,end)
        }
        rule named_window(level: usize) -> Node = name:ident() kw("as") window:window_spec(level) {
            Node::new(Kind::NamedWindow,vec![Node::name(name),window])
        }
        rule limit_value() -> Node = parameter() / start:position!() text:$(['0'..='9']+) !cont() end:position!() _ {? Node::number(text, start, end) }
        rule limit() -> Node = kw("limit") first:limit_value() tail:(kw("offset") value:limit_value() { (false, value) } / "," _ value:limit_value() { (true, value) })? {
            match tail {
                Some((true, count)) => Node::new(Kind::CommaLimit, vec![first, count]),
                Some((false, offset)) => Node::new(Kind::Limit, vec![first, offset]),
                None => Node::new(Kind::Limit, vec![first, Node::missing()]),
            }
        }
        rule lock_wait() -> Node
            = kw("skip") kw("locked") { Node::new(Kind::SkipLocked,vec![]) }
            / start:position!() (kw("nowait") / kw("no_wait")) { Node::new(Kind::LockWait,vec![]).at(start,start) }
            / kw("wait") start:position!() text:$(['0'..='9']+ ("." ['0'..='9']+)?) end:position!() _ {?
                let mut value=Node::number(text,start,end)?;
                value.kind=match value.kind { Kind::Int => Kind::LockWait, Kind::Number => Kind::LockWaitDecimal, _ => return Err("lock wait duration") };
                Ok(value)
            }
        rule lock() -> Node
            = kw("for") kw("update") wait:lock_wait()? { wait.unwrap_or_else(|| Node::new(Kind::LockWait,vec![]).value(u64::MAX)) }
            / kw("lock") kw("in") kw("share") kw("mode") { Node::new(Kind::LockWait,vec![]).value(u64::MAX) }
        rule hint_name() -> Name
            = start:position!() text:$("\"" ("\"\"" / !"\"" [_])+ "\"") end:position!() _ {
                Name { text:text[1..text.len()-1].replace("\"\"","\""),quoted:true,start,end }
            }
            / ident()
        rule hint_qb() -> Node = "@" _ name:hint_name() { Node::name(name) }
        rule hint_relation() -> Node = names:(hint_name() ++ ("." _)) qb:hint_qb()? {?
            Ok(Node::new(Kind::HintRelation,vec![Node::relation(names)?,Node::optional(qb)]))
        }
        rule hint_leading_item(level: usize) -> Node = depth(level)
            value:("(" _ value:hint_leading(level+1) ")" _ { value } / hint_relation()) { value }
        rule hint_leading(level: usize) -> Node = first:hint_leading_item(level) rest:(("," _)? value:hint_leading_item(level) { value })* {
            rest.into_iter().fold(first,|left,right| Node::new(Kind::Link,vec![left,right]))
        }
        rule hint_join_item() -> Node
            = "(" _ tables:(hint_relation() ++ (("," _)?)) ")" _ { Node::new(Kind::HintTables,tables) }
            / hint_relation()
        rule hint_option() -> Node
            = kw("leading") "(" _ qb:hint_qb()? tables:hint_leading(0) ")" _ { Node::new(Kind::Leading,vec![Node::optional(qb),tables]) }
            / kind:(kw("use_nl") { Kind::UseNl } / kw("no_use_nl") { Kind::NoUseNl }
                / kw("use_hash") { Kind::UseHash } / kw("no_use_hash") { Kind::NoUseHash }
                / kw("use_merge") { Kind::UseMerge } / kw("no_use_merge") { Kind::NoUseMerge })
                "(" _ qb:hint_qb()? tables:(hint_join_item() ++ (("," _)?)) ")" _ {
                    Node::new(kind,vec![Node::optional(qb),Node::new(Kind::JoinHintTables,tables)])
                }
            / kw("use_plan_cache") "(" _ policy:(kw("none") { 1 } / kw("default") { 2 }) ")" _ { Node::new(Kind::UsePlanCache,vec![]).value(policy) }
            / kw("no_rewrite") qb:("(" _ qb:hint_qb()? ")" _ { qb })? { Node::new(Kind::NoRewrite,vec![Node::optional(qb.flatten())]) }
            / kw("qb_name") "(" _ name:hint_name() ")" _ { Node::new(Kind::QbName,vec![Node::name(name)]) }
            / kw("opt_param") "(" _ name:string() ("," _)? value:(string() / number()) ")" _ { Node::new(Kind::OptParam,vec![name,value]) }
        rule hints() -> Vec<Node> = "/*+" _ options:(hint_option() ** (("," _)?)) "*/" _ { options }
        rule select(level: usize) -> Node = select_query(level) / values_query(level)
        rule select_query(level: usize) -> Node = depth(level) kw("select") hints:hints()? distinct:kw("distinct")? projection:(item(level) ++ ("," _))
            from:(kw("from") tables:(table(level) ++ ("," _)) { tables })?
            filter:(kw("where") value:expr(level) { value })?
            group_by:(kw("group") kw("by") values:expr_list(level) { values })?
            having:(kw("having") value:expr(level) { value })?
            windows:(kw("window") values:(named_window(level) ++ ("," _)) { values })?
            order_by:(kw("order") kw("by") values:(order_item(level) ++ ("," _)) { values })?
            limit:limit()? lock:lock()? {? Node::select(Select { distinct: distinct.is_some(), projection, from: from.unwrap_or_default(), filter, group_by: group_by.unwrap_or_default(), having, order_by: order_by.unwrap_or_default(), limit, lock, hints:hints.unwrap_or_default(),windows:windows.unwrap_or_default() }) }
        rule values_query(level: usize) -> Node = depth(level) kw("values") rows:((kw("row") value:row() { value }) ++ ("," _))
            order:(kw("order") kw("by") values:(order_item(level) ++ ("," _)) { values })? limit:limit()? {?
                let values=Node::new(Kind::Values,vec![Node::new(Kind::ValuesRows,rows)]);
                let projection=Node::new(Kind::Projection,vec![Node::new(Kind::Star,vec![])]);
                Node::select(Select { projection:vec![projection],from:vec![Node::new(Kind::Alias,vec![values,Node::missing()])],
                    order_by:order.unwrap_or_default(),limit,..Select::default() })
            }
        rule column() -> Node = start:position!() names:name() {? let end = names.last().unwrap().end; Node::column(names, false, start, end) }
        rule expr_or_default() -> Node = kw("default") { Node::new(Kind::Default, vec![]) } / expr(0)
        rule assignment() -> Node = column:column() "=" _ value:expr_or_default() { Node::new(Kind::Assignment, vec![column, value]) }
        rule assignments() -> Vec<Node> = values:(assignment() ++ ("," _)) { values }
        rule row() -> Node = "(" _ values:(expr_or_default() ** ("," _)) ")" _ {
            Node::new(Kind::ValueVector, if values.is_empty() { vec![Node::new(Kind::Empty, vec![])] } else { values })
        }
        rule insert() -> Node = replace:(kw("insert") { false } / kw("replace") { true }) hints:hints()?
            (kw("low_priority") / kw("high_priority"))? ignore:kw("ignore")? kw("into")? table:name()
            columns:("(" _ columns:(column() ** ("," _)) ")" _ { columns })?
            values:((kw("values") / kw("value")) rows:(row() ++ ("," _)) { Node::new(Kind::ValueList, rows) }
                / kw("set") values:assignments() { Node::new(Kind::AssignList, values) }
                / select(0))
            duplicate:(kw("on") kw("duplicate") kw("key") kw("update") values:assignments() { values })? {?
                if replace && (ignore.is_some() || duplicate.is_some()) { Err("REPLACE does not accept IGNORE or ON DUPLICATE KEY UPDATE") }
                else if values.kind == Kind::AssignList && columns.is_some() { Err("INSERT SET does not accept a column list") }
                else {
                    let table=Node::new(Kind::Table, vec![Node::relation(table)?, Node::missing(), Node::missing()]);
                    Ok(Node::insert(replace, ignore.is_some(), table, columns.unwrap_or_default(), values, duplicate.unwrap_or_default(), hints.unwrap_or_default()))
                }
            }
        rule update() -> Node = kw("update") hints:hints()? kw("low_priority")? ignore:kw("ignore")?
            tables:(table(0) ++ ("," _)) kw("set") values:assignments()
            filter:(kw("where") value:expr(0) { value })?
            order:(kw("order") kw("by") values:(order_item(0) ++ ("," _)) { values })?
            limit:limit()? {
                Node::new(Kind::Update, vec![Node::missing(), Node::new(Kind::TableReferences, tables),
                    Node::new(Kind::AssignList, values), Node::where_clause(filter), Node::order_clause(order.unwrap_or_default()),
                    Node::optional(limit), Node::missing(), Node::list(Kind::Hints,hints.unwrap_or_default()),
                    if ignore.is_some() { Node::new(Kind::Ignore, vec![]) } else { Node::missing() }, Node::missing(), Node::missing()])
            }
        rule delete() -> Node = kw("delete") hints:hints()? ignore:kw("ignore")? (kw("low_priority") / kw("quick"))?
            kw("from") table:relation(0) filter:(kw("where") value:expr(0) { value })?
            order:(kw("order") kw("by") values:(order_item(0) ++ ("," _)) { values })? limit:limit()? {
                Node::new(Kind::Delete, vec![Node::missing(), Node::new(Kind::DeleteTables, vec![Node::missing(),
                    Node::new(Kind::TableReferences, vec![table])]), Node::where_clause(filter),
                    Node::order_clause(order.unwrap_or_default()), Node::optional(limit),
                    Node::missing(), Node::list(Kind::Hints,hints.unwrap_or_default()), Node::missing(), Node::missing(),
                    if ignore.is_some() { Node::new(Kind::Ignore,vec![]) } else { Node::missing() }])
            }
        rule transaction_access() -> u64 = kw("read") value:(kw("only") { 2 } / kw("write") { 4 }) { value }
        rule transaction_options() -> u64
            = kw("with") kw("consistent") kw("snapshot") access:("," _ value:transaction_access() { value })? { 1|access.unwrap_or(0) }
            / access:transaction_access() snapshot:("," _ kw("with") kw("consistent") kw("snapshot"))? { access|u64::from(snapshot.is_some()) }
        rule transaction() -> Node
            = kw("savepoint") name:ident() { Node::new(Kind::Savepoint,vec![]).text(name.text).at(name.start,name.end) }
            / kw("release") kw("savepoint") name:ident() { Node::new(Kind::ReleaseSavepoint,vec![]).text(name.text).at(name.start,name.end) }
            / kw("rollback") kw("work")? kw("to") kw("savepoint")? name:ident() {
                Node::new(Kind::RollbackSavepoint,vec![]).text(name.text).at(name.start,name.end)
            }
            / options:(kw("begin") kw("work")? { 0 } / kw("start") kw("transaction") options:transaction_options()? { options.unwrap_or(0) }) {
                Node::new(Kind::Begin, vec![Node::new(Kind::HiddenInt, vec![]).value(options), Node::missing()])
            }
            / kw("commit") kw("work")? { Node::new(Kind::Commit, vec![Node::missing()]) }
            / kw("rollback") kw("work")? { Node::new(Kind::Rollback, vec![Node::missing()]) }
        rule if_not_exists() -> Node = kw("if") kw("not") kw("exists") { Node::new(Kind::IfNotExists, vec![]) }
        rule if_exists() -> Node = kw("if") kw("exists") { Node::new(Kind::IfExists, vec![]) }
        rule type_name() -> &'static str
            = kw("tinyint") { "tinyint" } / kw("smallint") { "smallint" } / kw("mediumint") { "mediumint" }
            / (kw("int") / kw("integer")) { "int" } / kw("bigint") { "bigint" }
            / kw("float") { "float" } / (kw("double") kw("precision")?) { "double" }
            / (kw("decimal") / kw("numeric") / kw("number")) { "decimal" }
            / kw("boolean") { "boolean" } / kw("bool") { "bool" }
            / kw("varchar") { "varchar" } / (kw("char") / kw("character")) { "char" }
            / kw("text") { "text" } / kw("blob") { "blob" }
            / kw("varbinary") { "varbinary" } / kw("binary") { "binary" }
            / kw("datetime") { "datetime" } / kw("timestamp") { "timestamp" }
            / kw("time") { "time" } / kw("date") { "date" } / kw("json") { "json" }
        rule type_size() -> Node = start:position!() text:$(['0'..='9']+) !cont() end:position!() _ {? Node::number(text, start, end) }
        rule cast_type() -> Node = start:position!() name:(kw("signed") kw("integer")? { "signed" }
                / kw("unsigned") kw("integer")? { "unsigned" } / kw("binary") { "binary" }
                / (kw("char") / kw("character")) { "char" }
                / (kw("decimal") / kw("numeric") / kw("number") / kw("fixed")) { "decimal" }
                / kw("double") { "double" } / kw("float") { "float" } / kw("json") { "json" }
                / kw("datetime") { "datetime" } / kw("date") { "date" } / kw("time") { "time" } / kw("year") { "year" })
            parameters:("(" _ values:(type_size() ++ ("," _)) ")" _ { values })? end:position!() {?
                let parameters=parameters.unwrap_or_default();
                let max=match name { "signed"|"unsigned"|"double"|"json" => 0,"decimal" => 2,_ => 1 };
                if parameters.len()>max || parameters.iter().any(|value| !matches!(value.kind,Kind::Int|Kind::Uint)) { Err("invalid cast type parameters") }
                else { Ok(Node::new(Kind::CastType,parameters).text(name.into()).at(start,end)) }
            }
        rule data_type() -> Node = start:position!() name:type_name()
            parameters:("(" _ values:(type_size() ++ ("," _)) ")" _ { values })?
            unsigned:kw("unsigned")? zerofill:kw("zerofill")? end:position!() {?
                Node::data_type(name, parameters.unwrap_or_default(), unsigned.is_some(), zerofill.is_some(), start, end)
            }
        rule column_attribute() -> Node
            = kw("not") kw("null") { Node::new(Kind::NotNull, vec![]) }
            / kw("null") { Node::new(Kind::Nullable, vec![]) }
            / kw("primary") kw("key") { Node::new(Kind::ColumnPrimaryKey, vec![]) }
            / kw("unique") kw("key")? { Node::new(Kind::UniqueKey, vec![]) }
            / kw("auto_increment") { Node::new(Kind::AutoIncrement, vec![]) }
            / kw("default") value:expr(0) { Node::new(Kind::ColumnDefault, vec![value]) }
            / kw("comment") value:string() { Node::new(Kind::ColumnComment, vec![value]) }
        rule table_element() -> Node
            = kw("primary") kw("key") "(" _ names:(ident() ++ ("," _)) ")" _ {
                Node::new(Kind::PrimaryKey, vec![Node::new(Kind::ColumnList, names.into_iter().map(Node::name).collect()), Node::missing(), Node::missing()])
            }
            / name:column() data_type:data_type() attributes:column_attribute()* {
                Node::new(Kind::ColumnDefinition, vec![name, data_type, Node::list(Kind::ColumnAttributes, attributes), Node::missing()])
            }
        rule ddl() -> Node
            = kw("create") (kw("database") / kw("schema")) absent:if_not_exists()? name:ident() {
                Node::new(Kind::CreateDatabase, vec![Node::optional(absent), Node::name(name), Node::missing()])
            }
            / kw("create") kw("table") absent:if_not_exists()? name:name()
                "(" _ elements:(table_element() ++ ("," _)) ")" _ {?
                Ok(Node::new(Kind::CreateTable, vec![Node::missing(), Node::optional(absent), Node::relation(name)?,
                    Node::new(Kind::TableElements, elements), Node::missing(), Node::missing(), Node::missing(), Node::missing()]))
            }
            / kw("drop") (kw("database") / kw("schema")) exists:if_exists()? name:ident() {
                Node::new(Kind::DropDatabase, vec![Node::optional(exists), Node::name(name)])
            }
            / kw("drop") (kw("table") / kw("tables")) exists:if_exists()? names:(name() ++ ("," _)) {?
                let tables=names.into_iter().map(Node::relation).collect::<Result<Vec<_>, _>>()?;
                Ok(Node::new(Kind::DropTable, vec![Node::missing(), Node::optional(exists), Node::new(Kind::TableList, tables)]))
            }
            / kw("use") name:ident() { Node::new(Kind::UseDatabase, vec![Node::name(name)]) }
        rule setting_value() -> Node
            = start:position!() value:(kw("on") { "ON" } / kw("binary") { "BINARY" }) { Node::new(Kind::String,vec![]).text(value.into()).at(start,start+value.len()) }
            / expr_or_default()
        rule charset_name() -> Node = kw("default") { Node::new(Kind::Default,vec![]) } / string() / name:ident() { Node::name(name) }
        rule variable_assignment() -> Node
            = kw("names") charset:charset_name() collation:(kw("collate") value:charset_name() { value })? {
                Node::new(Kind::SetNames,vec![charset,Node::optional(collation)])
            }
            / (kw("charset") / kw("character") kw("set")) charset:charset_name() { Node::new(Kind::SetCharset,vec![charset]) }
            / variable:user_variable() (":=" / "=" / kw("to")) _ value:expr(0) {
                Node::new(Kind::VariableValue,vec![variable,value]).value(2)
            }
            / scope:scope()? variable:(system_variable() / name:ident() { Node::name(name) })
                (":=" / "=" / kw("to")) _ value:setting_value() {
                let assignment_scope=scope.unwrap_or(if variable.kind==Kind::SystemVariable && variable.value!=0 { variable.value } else { 2 });
                let value=Node::setting_value(&variable,value);
                Node::new(Kind::VariableValue,vec![variable,value]).value(assignment_scope)
            }
        rule setting() -> Node = kw("set") assignments:(variable_assignment() ++ ("," _)) { Node::new(Kind::VariableSet,assignments) }
        rule show_condition() -> Node
            = kw("like") value:string() escape:(kw("escape") value:string() { value })? {?
                let escape=escape.unwrap_or_else(|| Node::new(Kind::String,vec![]).text("\\".into()));
                if escape.text.len()!=1 { Err("one-byte ESCAPE character") }
                else { Ok(Node::new(Kind::LikeClause,vec![value,escape])) }
            }
            / kw("where") value:expr(0) { Node::new(Kind::Where,vec![value]) }
        rule show_database() -> Node = (kw("from") / kw("in")) name:ident() { Node::new(Kind::FromList,vec![Node::name(name)]) }
        rule show_limit() -> Node = kw("limit") first:type_size() second:("," _ value:type_size() { value })? {
            match second { Some(second) => Node::new(Kind::ShowLimit,vec![first,second]),
                None => Node::new(Kind::ShowLimit,vec![Node::missing(),first]) }
        }
        rule show_body() -> Node
            = kw("grants") user:(kw("for") value:(kw("current_user") ("(" _ ")" _)? { Node::missing() } / account()) { value })? { Node::new(Kind::ShowGrants,vec![Node::optional(user),Node::missing()]) }
            / (kw("databases") / kw("schemas")) condition:show_condition()? { Node::new(Kind::ShowDatabases,vec![Node::optional(condition),Node::missing()]) }
            / full:kw("full")? kw("tables") database:show_database()? condition:show_condition()? {
                Node::new(Kind::ShowTables,vec![Node::optional(database),Node::optional(condition),Node::new(Kind::Int,vec![]).value(u64::from(full.is_some()))])
            }
            / full:kw("full")? (kw("columns") / kw("fields")) (kw("from") / kw("in")) table:name() database:show_database()? condition:show_condition()? {?
                Ok(Node::new(Kind::ShowColumns,vec![Node::new(Kind::Int,vec![]).value(u64::from(full.is_some())),Node::relation(table)?,Node::optional(database),Node::optional(condition)]))
            }
            / scope:scope()? kw("variables") condition:show_condition()? {
                Node::new(Kind::ShowVariables,vec![Node::new(Kind::Int,vec![]).value(scope.unwrap_or(0)),Node::optional(condition)])
            }
            / kw("create") kw("table") table:name() {? Ok(Node::new(Kind::ShowCreateTable,vec![Node::relation(table)?])) }
            / kw("create") (kw("database") / kw("schema")) absent:if_not_exists()? name:ident() { Node::new(Kind::ShowCreateDatabase,vec![Node::optional(absent),Node::name(name)]) }
            / kind:(kw("warnings") { Kind::ShowWarnings } / kw("errors") { Kind::ShowErrors }) limit:show_limit()? { Node::new(kind,vec![Node::optional(limit)]) }
        rule show() -> Node
            = kw("show") value:show_body() { value }
            / (kw("describe") / kw("desc")) table:name() {? Ok(Node::new(Kind::ShowColumns,vec![Node::new(Kind::Int,vec![]),Node::relation(table)?,Node::missing(),Node::missing()])) }
        rule config_scope() -> u64 = kw("scope") "=" _ value:(kw("memory") { 0 } / kw("spfile") { 1 } / kw("both") { 2 }) { value }
        rule config_value() -> Node = string() / bool_null() / system_variable() / number()
            / start:position!() "-" _ value:number() {? Node::unary("-",value,start) }
        rule config_assignment() -> Node = name:ident() "=" _ value:config_value()
            comment:(kw("comment") value:string() { value })? scope:config_scope()? {
                Node::new(Kind::ConfigAction,vec![Node::name(name),value,Node::optional(comment),Node::missing()]).value(scope.unwrap_or(2))
            }
        rule tracepoint_option() -> Node
            = kw("tp_name") ("=" _)? name:(string() / name:ident() { Node::name(name) }) { Node::new(Kind::TracepointName,vec![name]) }
            / kind:(kw("tp_no") { Kind::TracepointNumber } / kw("occur") { Kind::TracepointOccurrence }
                / kw("frequency") { Kind::TracepointFrequency } / kw("error_code") { Kind::TracepointError }
                / kw("match") { Kind::TracepointMatch }) ("=" _)? value:type_size() { Node::new(kind,vec![value]) }
        rule cache_type() -> Node = value:(kw("plan") { "plan" } / kw("pl") { "pl" } / kw("ps") { "ps" } / kw("lib") { "lib" }) {
            Node::new(Kind::CacheType,vec![]).text(value.into())
        }
        rule flush_cache() -> Node = kw("flush") value:(
            cache:cache_type() kw("cache")
            namespace:(kw("namespace") ("=" _)? value:string() { Node::new(Kind::CacheNamespace,vec![value]) })?
            id:(kw("sql_id") ("=" _)? value:string() { Node::new(Kind::SqlId,vec![value]) }
                / kw("schema_id") ("=" _)? value:limit_value() { Node::new(Kind::SchemaId,vec![value]) })?
            databases:(kw("databases") ("=" _)? value:string() { Node::new(Kind::DatabaseList,vec![value]) })? kw("global")? {
                Node::new(Kind::FlushCache,vec![cache,Node::optional(namespace),Node::optional(id),Node::optional(databases)])
            }
            / kw("sql") cache:cache_type() kw("global")? { Node::new(Kind::FlushCache,vec![cache,Node::missing(),Node::missing(),Node::missing()]) }
        ) { value }
        rule alter_system() -> Node = kw("alter") kw("system") value:(
            flush_cache()
            / kw("set_tp") options:(tracepoint_option() ** ("," _)) { Node::new(Kind::SetTracepoint,vec![Node::list(Kind::TracepointOptions,options),Node::missing()]) }
            / kw("reset") actions:((name:ident() scope:config_scope()? { Node::new(Kind::ConfigAction,vec![Node::name(name)]).value(scope.unwrap_or(2)) }) ++ ("," _)) {
                Node::new(Kind::ResetConfig,vec![Node::new(Kind::ConfigList,actions)])
            }
            / kw("set")? actions:(config_assignment() ++ ("," _)) { Node::new(Kind::SetConfig,vec![Node::new(Kind::ConfigList,actions)]) }
        ) { value }
        rule account_name() -> Node = string() / name:ident() { Node::name(name) }
        rule account() -> Node = name:account_name() host:user_variable()? {
            Node::new(Kind::UserHost,vec![name,Node::optional(host)])
        }
        rule password() -> Node = value:string() { Node { kind:Kind::Password,..value } }
        rule authentication() -> (Node,Node,Node) = kw("identified")
            plugin:(kw("with") value:account_name() { value })?
            password:(kw("by") encoded:kw("password")? value:password() { (value,u64::from(encoded.is_none())) }
                / kw("as") value:password() { (value,0) }) {
                (password.0,Node::new(Kind::Bool,vec![]).value(password.1),Node::optional(plugin))
            }
        rule user_spec() -> Node = name:account_name() host:user_variable()? auth:authentication()? {
            let (password,encrypted,plugin)=auth.unwrap_or_else(|| (Node::missing(),Node::new(Kind::Bool,vec![]),Node::missing()));
            Node::new(Kind::UserSpec,vec![name,password,encrypted,Node::optional(host),plugin])
        }
        rule privilege_name() -> &'static str
            = kw("create") kw("ai") kw("model") { "create ai model" }
            / kw("alter") kw("ai") kw("model") { "alter ai model" }
            / kw("drop") kw("ai") kw("model") { "drop ai model" }
            / kw("access") kw("ai") kw("model") { "access ai model" }
            / kw("create") kw("user") { "create user" }
            / kw("grant") kw("option") { "grant option" }
            / kw("create") kw("view") { "create view" }
            / kw("show") kw("view") { "show view" }
            / kw("show") kw("databases") { "show databases" }
            / kw("replication") kw("slave") { "replication slave" }
            / kw("replication") kw("client") { "replication client" }
            / kw("alter") kw("system") { "alter system" }
            / kw("alter") kw("routine") { "alter routine" }
            / kw("create") kw("routine") { "create routine" }
            / kw("lock") kw("tables") { "lock tables" }
            / kw("create") kw("role") { "create role" }
            / kw("drop") kw("role") { "drop role" }
            / kw("alter") { "alter" }
            / kw("create") { "create" }
            / kw("delete") { "delete" }
            / kw("drop") { "drop" }
            / kw("insert") { "insert" }
            / kw("update") { "update" }
            / kw("select") { "select" }
            / kw("index") { "index" }
            / kw("super") { "super" }
            / kw("process") { "process" }
            / kw("file") { "file" }
            / kw("execute") { "execute" }
            / kw("shutdown") { "shutdown" }
            / kw("reload") { "reload" }
            / kw("references") { "references" }
            / kw("trigger") { "trigger" }
            / kw("read") { "read" }
            / kw("write") { "write" }
            / kw("usage") { "usage" }
        rule privilege() -> Node = name:privilege_name()
            columns:("(" _ names:(ident() ++ ("," _)) ")" _ { names })? {?
                if columns.is_some() && !matches!(name,"select"|"insert"|"update"|"references") { return Err("column privilege"); }
                let children=columns.map(|names| vec![Node::new(Kind::ColumnList,names.into_iter().map(Node::name).collect())]).unwrap_or_default();
                Ok(Node::new(Kind::Privilege,children).text(name.into()))
            }
        rule privileges() -> Vec<Node> = kw("all") kw("privileges")? { vec![Node::new(Kind::Privilege,vec![]).text("all".into())] }
            / values:(privilege() ++ ("," _)) { values }
        rule privilege_level() -> Node = first:("*" _ { Node::new(Kind::Star,vec![]) } / name:ident() { Node::name(name) })
            second:("." _ value:("*" _ { Node::new(Kind::Star,vec![]) } / name:ident() { Node::name(name) }) { value })? {
                match second { Some(second) => Node::new(Kind::PrivilegeLevel,vec![first,second]),None => first }
            }
        rule privilege_object() -> Node = value:(kw("table") { 1 } / kw("procedure") { 2 } / kw("function") { 3 }) {
            Node::new(Kind::PrivilegeObject,vec![]).value(value)
        }
        rule accounts() -> Node = values:(account() ++ ("," _)) { Node::new(Kind::Users,values) }
        rule account_statement() -> Node
            = kw("create") kw("user") absent:if_not_exists()? users:(user_spec() ++ ("," _)) {
                Node::new(Kind::CreateUser,vec![Node::optional(absent),Node::new(Kind::Users,users),Node::missing(),Node::missing()])
            }
            / kw("drop") kw("user") exists:if_exists()? users:accounts() { Node::new(Kind::DropUser,vec![users,Node::optional(exists)]) }
            / kw("alter") kw("user") user:account() auth:authentication() {
                Node::new(Kind::SetPassword,vec![user,auth.0,auth.1,Node::missing(),auth.2])
            }
            / kw("set") kw("password") user:(kw("for") user:account() { user })? "=" _
                auth:(kw("password") "(" _ password:password() ")" _ { (password,1) } / password:password() { (password,0) }) {
                    Node::new(Kind::SetPassword,vec![Node::optional(user),auth.0,Node::new(Kind::Bool,vec![]).value(auth.1),Node::missing(),Node::missing()])
                }
            / kw("grant") privileges:privileges() kw("on") object:privilege_object()? level:privilege_level()
                kw("to") users:(user_spec() ++ ("," _)) option:(kw("with") kw("grant") kw("option"))? {
                    let mut privileges=privileges;
                    if option.is_some() { privileges.push(Node::new(Kind::Privilege,vec![]).text("grant option".into())); }
                    Node::new(Kind::Grant,vec![Node::new(Kind::Privileges,privileges),Node::optional(object),level,Node::new(Kind::Users,users)])
                }
            / kw("revoke") exists:if_exists()? kw("all") kw("privileges")? "," _ kw("grant") kw("option") kw("from") users:accounts() {
                Node::new(Kind::RevokeAll,vec![users,Node::optional(exists),Node::missing()])
            }
            / kw("revoke") exists:if_exists()? privileges:privileges() kw("on") object:privilege_object()? level:privilege_level() kw("from") users:accounts() {
                Node::new(Kind::Revoke,vec![Node::new(Kind::Privileges,privileges),Node::optional(object),level,users,Node::optional(exists),Node::missing()])
            }
        rule prepared_statement() -> Node
            = kw("prepare") name:ident() kw("from") source:(string() / value:user_variable() { Node::new(Kind::GetUserVariable,vec![value]) }) {
                Node::new(Kind::Prepare,vec![Node::name(name),source])
            }
            / kw("execute") name:ident() args:(kw("using") args:((value:user_variable() { Node::new(Kind::GetUserVariable,vec![value]) }) ++ ("," _)) { args })? {
                Node::new(Kind::Execute,vec![Node::name(name),Node::list(Kind::ExecuteArguments,args.unwrap_or_default())])
            }
            / (kw("deallocate") / kw("drop")) kw("prepare") name:ident() { Node::new(Kind::Deallocate,vec![Node::name(name)]) }
        pub rule query() -> Node = _ value:(select(0) / insert() / update() / delete() / transaction() / ddl() / account_statement() / setting() / show() / alter_system() / prepared_statement()) (";" _)* { value }
            / text:$(_ (";" _)*) { Node::new(Kind::EmptyQuery,vec![]).value(u64::from(text.chars().any(|ch| !ch.is_ascii_whitespace() && ch!=';'))) }
    }
}
