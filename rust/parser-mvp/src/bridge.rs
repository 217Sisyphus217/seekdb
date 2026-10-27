use crate::{ast::Expr, children, select, Parser};
use std::sync::LazyLock;

#[cxx::bridge(namespace = "oceanbase::sql::peg")]
mod ffi {
    struct IntegerProjection {
        value: i64,
        literal: String,
        expression: String,
        offset: usize,
        alias: String,
    }

    struct IntegerSelect {
        columns: Vec<IntegerProjection>,
        error: String,
    }

    extern "Rust" {
        fn parse_integer_select(sql: &[u8]) -> IntegerSelect;
    }
}

// This integration slice tests the existing resolver/executor with integer projections.
// Unsupported queries fail explicitly; they are never passed to the legacy parser.
fn integer_columns(sql: &str) -> Result<Vec<ffi::IntegerProjection>, String> {
    static PARSER: LazyLock<Result<Parser, String>> = LazyLock::new(Parser::new);
    let tree = PARSER.as_ref().map_err(Clone::clone)?.query(sql)?;
    let projection = children(tree.clone())
        .find(|p| p.as_rule() == "projection")
        .unwrap();
    let sources = children(projection).map(|item| children(item).next().unwrap());
    let query = select(tree);
    if query.distinct
        || !query.from.is_empty()
        || query.filter.is_some()
        || !query.group_by.is_empty()
        || query.having.is_some()
        || !query.order_by.is_empty()
        || query.limit.is_some()
    {
        return Err(
            "PEG integration MVP supports only SELECT integer literals with optional aliases"
                .into(),
        );
    }
    query
        .projection
        .into_iter()
        .zip(sources)
        .map(|(item, source)| {
            let Expr::Number { text } = item.expression else {
                return Err("PEG integration MVP requires integer literals".into());
            };
            let value = text
                .parse::<i64>()
                .map_err(|_| "PEG integration MVP requires signed 64-bit integer literals")?;
            Ok(ffi::IntegerProjection {
                value,
                literal: text,
                expression: source.as_str().to_owned(),
                offset: source.as_span().start(),
                alias: item.alias.unwrap_or_default(),
            })
        })
        .collect()
}

fn parse_integer_select(sql: &[u8]) -> ffi::IntegerSelect {
    let columns = std::str::from_utf8(sql)
        .map_err(|e| e.to_string())
        // seekdb can enter Rust on a SMART_CALL stack that stacker does not own.
        // Keep pest's stack guard; register a bounded Rust stack when needed.
        .and_then(|sql| stacker::maybe_grow(256 * 1024, 2 * 1024 * 1024, || integer_columns(sql)));
    match columns {
        Ok(columns) => ffi::IntegerSelect {
            columns,
            error: String::new(),
        },
        Err(error) => ffi::IntegerSelect {
            columns: Vec::new(),
            error,
        },
    }
}
