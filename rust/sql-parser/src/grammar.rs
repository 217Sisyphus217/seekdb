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
        rule marker() = ("/*+parser_mvp*/" _)?
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
        rule atom(level: usize) -> Node
            = depth(level) start:position!() "(" _ query:select(level+1) ")" end:position!() _
                {? query.expression(start, end) }
            / depth(level) start:position!() "(" _ value:expr(level+1) ")" end:position!() _
                {? Node::new(Kind::Parenthesized, vec![value]).expression(start, end) }
            / depth(level) start:position!() kw("exists") "(" _ query:select(level+1) ")" end:position!() _
                {? Node::new(Kind::Exists, vec![query]).expression(start, end) }
            / start:position!() "*" end:position!() _ {? Node::new(Kind::Star, vec![]).expression(start, end) }
            / number() / string() / bool_null() / parameter()
            / depth(level) start:position!() parts:expression_name() tail:("." _ "*" end:position!() _ { (None, end) }
                / "(" _ distinct:kw("distinct")? args:expr_list(level+1)? ")" end:position!() _
                    { (Some((distinct.is_some(), args.unwrap_or_default())), end) })?
                {? match tail {
                    None => { let end=parts.last().unwrap().end; Node::column(parts, false, start, end) },
                    Some((None, end)) => Node::column(parts, true, start, end),
                    Some((Some((distinct, arguments)), end)) => Node::call(parts, distinct, arguments, start, end),
                } }
        rule unary(level: usize) -> Node
            = ops:(start:position!() op:$(['+' | '-' | '!' | '~']) _ { (start, op) }
                / enabled(mode.high_not_precedence) start:position!() kw("not") { (start, "not") })* value:atom(level)
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
        rule item(level: usize) -> Node = expression:expr(level) alias:alias()? {? Node::projection(expression, alias) }
        rule relation(level: usize) -> Node = source:("(" _ query:select(level+1) ")" _ { query }
            / name:name() {? Node::relation(name) }) alias:alias()? {? Node::table(source, alias) }
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
        rule limit_value() -> Node = parameter() / start:position!() text:$(['0'..='9']+) !cont() end:position!() _ {? Node::number(text, start, end) }
        rule limit() -> Node = kw("limit") first:limit_value() tail:(kw("offset") value:limit_value() { (false, value) } / "," _ value:limit_value() { (true, value) })? {
            match tail {
                Some((true, count)) => Node::new(Kind::CommaLimit, vec![first, count]),
                Some((false, offset)) => Node::new(Kind::Limit, vec![first, offset]),
                None => Node::new(Kind::Limit, vec![first, Node::missing()]),
            }
        }
        rule select(level: usize) -> Node = depth(level) kw("select") marker() distinct:kw("distinct")? projection:(item(level) ++ ("," _))
            from:(kw("from") tables:(table(level) ++ ("," _)) { tables })?
            filter:(kw("where") value:expr(level) { value })?
            group_by:(kw("group") kw("by") values:expr_list(level) { values })?
            having:(kw("having") value:expr(level) { value })?
            order_by:(kw("order") kw("by") values:(order_item(level) ++ ("," _)) { values })?
            limit:limit()? {? Node::select(Select { distinct: distinct.is_some(), projection, from: from.unwrap_or_default(), filter, group_by: group_by.unwrap_or_default(), having, order_by: order_by.unwrap_or_default(), limit }) }
        rule column() -> Node = start:position!() names:name() {? let end = names.last().unwrap().end; Node::column(names, false, start, end) }
        rule expr_or_default() -> Node = kw("default") { Node::new(Kind::Default, vec![]) } / expr(0)
        rule assignment() -> Node = column:column() "=" _ value:expr_or_default() { Node::new(Kind::Assignment, vec![column, value]) }
        rule assignments() -> Vec<Node> = values:(assignment() ++ ("," _)) { values }
        rule row() -> Node = "(" _ values:(expr_or_default() ** ("," _)) ")" _ {
            Node::new(Kind::ValueVector, if values.is_empty() { vec![Node::new(Kind::Empty, vec![])] } else { values })
        }
        rule insert() -> Node = replace:(kw("insert") { false } / kw("replace") { true }) marker()
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
                    Ok(Node::insert(replace, ignore.is_some(), table, columns.unwrap_or_default(), values, duplicate.unwrap_or_default()))
                }
            }
        rule update() -> Node = kw("update") marker() kw("low_priority")? ignore:kw("ignore")?
            tables:(table(0) ++ ("," _)) kw("set") values:assignments()
            filter:(kw("where") value:expr(0) { value })?
            order:(kw("order") kw("by") values:(order_item(0) ++ ("," _)) { values })?
            limit:limit()? {
                Node::new(Kind::Update, vec![Node::missing(), Node::new(Kind::TableReferences, tables),
                    Node::new(Kind::AssignList, values), Node::where_clause(filter), Node::order_clause(order.unwrap_or_default()),
                    Node::optional(limit), Node::missing(), Node::missing(),
                    if ignore.is_some() { Node::new(Kind::Ignore, vec![]) } else { Node::missing() }, Node::missing(), Node::missing()])
            }
        rule delete() -> Node = kw("delete") marker() kw("ignore")? (kw("low_priority") / kw("quick"))?
            kw("from") table:relation(0) filter:(kw("where") value:expr(0) { value })?
            order:(kw("order") kw("by") values:(order_item(0) ++ ("," _)) { values })? limit:limit()? {
                Node::new(Kind::Delete, vec![Node::missing(), Node::new(Kind::DeleteTables, vec![Node::missing(),
                    Node::new(Kind::TableReferences, vec![table])]), Node::where_clause(filter),
                    Node::order_clause(order.unwrap_or_default()), Node::optional(limit),
                    Node::missing(), Node::missing(), Node::missing(), Node::missing(), Node::missing()])
            }
        rule transaction() -> Node
            = (kw("begin") marker() kw("work")? / kw("start") marker() kw("transaction")) {
                Node::new(Kind::Begin, vec![Node::new(Kind::Int, vec![]), Node::missing()])
            }
            / kw("commit") marker() kw("work")? { Node::new(Kind::Commit, vec![Node::missing()]) }
            / kw("rollback") marker() kw("work")? { Node::new(Kind::Rollback, vec![Node::missing()]) }
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
            = kw("create") marker() (kw("database") / kw("schema")) absent:if_not_exists()? name:ident() {
                Node::new(Kind::CreateDatabase, vec![Node::optional(absent), Node::name(name), Node::missing()])
            }
            / kw("create") marker() kw("table") absent:if_not_exists()? name:name()
                "(" _ elements:(table_element() ++ ("," _)) ")" _ {?
                Ok(Node::new(Kind::CreateTable, vec![Node::missing(), Node::optional(absent), Node::relation(name)?,
                    Node::new(Kind::TableElements, elements), Node::missing(), Node::missing(), Node::missing(), Node::missing()]))
            }
            / kw("drop") marker() (kw("database") / kw("schema")) exists:if_exists()? name:ident() {
                Node::new(Kind::DropDatabase, vec![Node::optional(exists), Node::name(name)])
            }
            / kw("drop") marker() (kw("table") / kw("tables")) exists:if_exists()? names:(name() ++ ("," _)) {?
                let tables=names.into_iter().map(Node::relation).collect::<Result<Vec<_>, _>>()?;
                Ok(Node::new(Kind::DropTable, vec![Node::missing(), Node::optional(exists), Node::new(Kind::TableList, tables)]))
            }
            / kw("use") marker() name:ident() { Node::new(Kind::UseDatabase, vec![Node::name(name)]) }
        pub rule query() -> Node = _ value:(select(0) / insert() / update() / delete() / transaction() / ddl()) (";" _)? { value }
    }
}
