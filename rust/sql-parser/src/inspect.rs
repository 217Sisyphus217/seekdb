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

use crate::bridge::{
    self,
    ffi::{ColumnReference, ExpressionInfo, ParseMode},
};
use crate::node::{Kind, Node};
use crate::SqlMode;

pub(crate) fn inspect_expression(sql: &[u8], mode: SqlMode) -> ExpressionInfo {
    let parsed = bridge::parse_sql(sql, mode, ParseMode::Expression);
    let mut root = &parsed.root;
    while root.kind == Kind::Parenthesized {
        root = &root.children[0];
    }
    let function_name = match root.kind {
        Kind::Function | Kind::Cast | Kind::StringConcat => root.children[0].text.clone(),
        _ => String::new(),
    };
    let mut columns = Vec::new();
    if parsed.error.is_empty() {
        collect_columns(root, &mut columns);
    }
    ExpressionInfo {
        root_kind: root.kind,
        function_name,
        columns,
        error: parsed.error,
        error_offset: parsed.error_offset,
    }
}

fn collect_columns(node: &Node, columns: &mut Vec<ColumnReference>) {
    if node.kind == Kind::Column {
        columns.push(ColumnReference {
            parts: node
                .children
                .iter()
                .filter(|child| child.kind == Kind::Ident)
                .map(|child| child.text.clone())
                .collect(),
            wildcard: node.children.iter().any(|child| child.kind == Kind::Star),
        });
    } else if node.kind == Kind::ObjectAccess
        && node.children[0].kind == Kind::Ident
        && node.children[1].kind == Kind::Missing
    {
        columns.push(ColumnReference {
            parts: vec![node.children[0].text.clone()],
            wildcard: false,
        });
    }
    for child in &node.children {
        collect_columns(child, columns);
    }
}
