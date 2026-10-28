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

use crate::{
    bridge::ffi::{ViewCheck, ViewDefinition},
    node::Node,
};

pub(crate) fn view_definition(node: &Node) -> ViewDefinition<'_> {
    ViewDefinition {
        relation: &node.children[0],
        columns: &node.children[1].children,
        query: &node.children[3],
        replace: node.children[4].kind == crate::node::Kind::IfNotExists,
        alter: node.value != 0,
        check: match node.children[5].value {
            0 => ViewCheck::None,
            1 => ViewCheck::Local,
            2 => ViewCheck::Cascaded,
            _ => unreachable!(),
        },
    }
}

use crate::bridge::ffi::{
    ColumnDefinition, ColumnGeneration, ColumnPosition, DropBehavior, ForeignKeyDefinition,
    IndexDefinition, IndexKind, IndexMethod, PartitionDefinition, PartitionElement, PartitionKind,
    PartitionValues, ReferenceAction, ReferenceEvent, ReferentialAction, TableDefinition,
    TableOrganization, TableRowFormat, TableSelectMode,
};
use crate::{
    node::Kind,
    query::{child, list},
};

pub(crate) fn table_definition(node: &Node) -> TableDefinition<'_> {
    TableDefinition {
        relation: &node.children[2],
        elements: &node.children[3].children,
        options: &node.children[4].children,
        hints: list(node, Kind::Hints),
        temporary: node.children[0].kind == Kind::Temporary,
        if_not_exists: node.children[1].kind == Kind::IfNotExists,
        select_mode: if node.children.len() == 8 {
            TableSelectMode::None
        } else {
            match node.children[10].kind {
                Kind::Ignore => TableSelectMode::Ignore,
                Kind::Replace => TableSelectMode::Replace,
                _ => TableSelectMode::Select,
            }
        },
        has_partition: node.children[5].kind != Kind::Missing,
    }
}
pub(crate) fn table_definition_query(node: &Node) -> &Node {
    &node.children[8]
}
pub(crate) fn table_definition_partition(node: &Node) -> &Node {
    &node.children[5].children[0]
}
pub(crate) fn table_like_relation(node: &Node) -> &Node {
    &node.children[2]
}
pub(crate) fn table_like_source(node: &Node) -> &Node {
    &node.children[3]
}
pub(crate) fn table_like_temporary(node: &Node) -> bool {
    node.children[0].kind == Kind::Temporary
}
pub(crate) fn table_like_if_not_exists(node: &Node) -> bool {
    node.children[1].kind == Kind::IfNotExists
}

pub(crate) fn column_definition(node: &Node) -> ColumnDefinition<'_> {
    let generated = node.children.len() == 7;
    let position = &node.children[if generated { 5 } else { 3 }];
    ColumnDefinition {
        name: &node.children[0],
        data_type: &node.children[1],
        attributes: &node.children[2].children,
        generation: if !generated {
            ColumnGeneration::None
        } else if node.children[4].kind == Kind::StoredColumn {
            ColumnGeneration::Stored
        } else {
            ColumnGeneration::Virtual
        },
        position: match position.kind {
            Kind::ColumnFirst => ColumnPosition::First,
            Kind::ColumnBefore => ColumnPosition::Before,
            Kind::ColumnAfter => ColumnPosition::After,
            _ => ColumnPosition::None,
        },
    }
}
pub(crate) fn column_generation_expression(node: &Node) -> &Node {
    &node.children[3]
}
pub(crate) fn column_position_name(node: &Node) -> &Node {
    &node.children[if node.children.len() == 7 { 5 } else { 3 }].children[0]
}
pub(crate) fn column_attribute_expression(attribute: &Node) -> &Node {
    &attribute.children[0]
}
pub(crate) fn column_attribute_text(attribute: &Node) -> &str {
    if attribute.kind == Kind::Collation {
        &attribute.text
    } else {
        &attribute.children[0].text
    }
}
pub(crate) fn column_srid(attribute: &Node) -> u64 {
    attribute.children[0].value
}

pub(crate) fn index_definition(node: &Node) -> IndexDefinition<'_> {
    let standalone = node.kind == Kind::CreateIndex;
    let offset = usize::from(standalone);
    let value = if standalone {
        node.children[0].value
    } else {
        node.value
    };
    IndexDefinition {
        columns: &node.children[1 + offset].children,
        options: &node.children[2 + offset].children,
        kind: match value {
            0 => IndexKind::Normal,
            1 => IndexKind::Unique,
            2 => IndexKind::Spatial,
            3 => IndexKind::Fulltext,
            6 => IndexKind::Vector,
            _ => unreachable!(),
        },
        method: if node.children[3 + offset].kind == Kind::UsingHash {
            IndexMethod::Hash
        } else {
            IndexMethod::Default
        },
        has_name: node.children[0].kind != Kind::Missing,
        has_partition: node.children[4 + offset].kind != Kind::Missing,
        if_not_exists: standalone && node.children[7].kind == Kind::IfNotExists,
    }
}
pub(crate) fn index_definition_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn index_definition_table(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn index_definition_partition(node: &Node) -> &Node {
    &node.children[if node.kind == Kind::CreateIndex { 5 } else { 4 }].children[0]
}
pub(crate) fn index_column_expression(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn index_column_has_prefix(node: &Node) -> bool {
    node.children[1].kind != Kind::Missing
}
pub(crate) fn index_column_prefix(node: &Node) -> u64 {
    node.children[1].value
}
pub(crate) fn index_column_descending(node: &Node) -> bool {
    node.children[2].kind == Kind::Desc
}
pub(crate) fn primary_key_columns(node: &Node) -> &[Node] {
    &node.children[0].children
}
pub(crate) fn primary_key_method(node: &Node) -> IndexMethod {
    if node.children[1].kind == Kind::UsingHash {
        IndexMethod::Hash
    } else {
        IndexMethod::Default
    }
}
pub(crate) fn primary_key_options(node: &Node) -> &[Node] {
    if node.children[1].kind == Kind::TableOptions {
        &node.children[1].children
    } else {
        &[]
    }
}

pub(crate) fn partition_definition(node: &Node) -> PartitionDefinition<'_> {
    let hash = matches!(node.kind, Kind::HashPartition | Kind::KeyPartition);
    let count = &node.children[if hash { 1 } else { 3 }];
    PartitionDefinition {
        kind: match node.kind {
            Kind::HashPartition => PartitionKind::Hash,
            Kind::KeyPartition => PartitionKind::Key,
            Kind::RangePartition => PartitionKind::Range,
            Kind::RangeColumnsPartition => PartitionKind::RangeColumns,
            Kind::ListPartition => PartitionKind::List,
            Kind::ListColumnsPartition => PartitionKind::ListColumns,
            _ => unreachable!(),
        },
        expression: &node.children[0],
        elements: &node.children[if hash { 2 } else { 1 }].children,
        has_count: count.kind != Kind::Missing,
        count: count.value,
        has_subpartition: node.children[if hash { 3 } else { 2 }].kind != Kind::Missing,
        is_template: node.children[4].kind != Kind::Missing,
    }
}
pub(crate) fn partition_subpartition(node: &Node) -> &Node {
    &node.children[if matches!(node.kind, Kind::HashPartition | Kind::KeyPartition) {
        3
    } else {
        2
    }]
}
pub(crate) fn partition_element(node: &Node) -> PartitionElement<'_> {
    PartitionElement {
        name: &node.children[0],
        values: &node.children[1].children,
        value_kind: match node.children[1].kind {
            Kind::Missing => PartitionValues::None,
            Kind::Default => PartitionValues::Default,
            _ if node.kind == Kind::RangePartitionElement => PartitionValues::Range,
            _ => PartitionValues::List,
        },
        subpartitions: &node.children[3].children,
    }
}
pub(crate) fn check_constraint_has_name(node: &Node) -> bool {
    node.children[0].kind != Kind::Missing && node.children[0].children[0].kind != Kind::Missing
}
pub(crate) fn check_constraint_name(node: &Node) -> &Node {
    &node.children[0].children[0]
}
pub(crate) fn check_constraint_expression(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn check_constraint_enforced(node: &Node) -> bool {
    node.children[2].kind == Kind::Enforced
}
pub(crate) fn foreign_key_definition(node: &Node) -> ForeignKeyDefinition<'_> {
    ForeignKeyDefinition {
        columns: &node.children[0].children,
        parent: &node.children[1],
        parent_columns: &node.children[2].children,
        actions: &node.children[3].children,
        has_name: node.children[4].kind != Kind::Missing
            && node.children[4].children[0].kind != Kind::Missing,
        has_index_name: node.children[5].kind != Kind::Missing,
    }
}
pub(crate) fn foreign_key_name(node: &Node) -> &Node {
    &node.children[4].children[0]
}
pub(crate) fn foreign_key_index_name(node: &Node) -> &Node {
    &node.children[5]
}
pub(crate) fn referential_action(node: &Node) -> ReferentialAction {
    ReferentialAction {
        event: if node.value != 0 {
            ReferenceEvent::Delete
        } else {
            ReferenceEvent::Update
        },
        action: match node.children[0].kind {
            Kind::Restrict => ReferenceAction::Restrict,
            Kind::Cascade => ReferenceAction::Cascade,
            Kind::SetNull => ReferenceAction::SetNull,
            Kind::NoAction => ReferenceAction::NoAction,
            Kind::SetDefault => ReferenceAction::SetDefault,
            _ => unreachable!(),
        },
    }
}
pub(crate) fn table_option_text(node: &Node) -> &str {
    if node.kind == Kind::Collation {
        &node.text
    } else {
        &node.children[0].text
    }
}
pub(crate) fn table_option_number(node: &Node) -> u64 {
    node.children[0].value
}
pub(crate) fn table_option_names(node: &Node) -> &[Node] {
    &node.children
}
pub(crate) fn table_option_properties(node: &Node) -> &[Node] {
    &node.children
}
pub(crate) fn table_option_charset(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn table_option_organization(node: &Node) -> TableOrganization {
    if node.children[0].kind == Kind::OrganizationIndex {
        TableOrganization::Index
    } else {
        TableOrganization::Heap
    }
}
pub(crate) fn table_option_row_format(node: &Node) -> TableRowFormat {
    match node.children[0].value {
        1 => TableRowFormat::Redundant,
        2 => TableRowFormat::Compact,
        3 => TableRowFormat::Dynamic,
        4 => TableRowFormat::Compressed,
        5 => TableRowFormat::Condensed,
        _ => unreachable!(),
    }
}
pub(crate) fn index_option_global(node: &Node) -> bool {
    node.children[0].value != 0
}

pub(crate) fn database_name(node: &Node) -> &Node {
    &node.children[if matches!(node.kind, Kind::CreateDatabase | Kind::DropDatabase) {
        1
    } else {
        0
    }]
}
pub(crate) fn database_options(node: &Node) -> &[Node] {
    list(node, Kind::DatabaseOptions)
}
pub(crate) fn database_if_not_exists(node: &Node) -> bool {
    node.children[0].kind == Kind::IfNotExists
}
pub(crate) fn drop_if_exists(node: &Node) -> bool {
    child(node, Kind::IfExists).is_some()
}
pub(crate) fn drop_temporary(node: &Node) -> bool {
    child(node, Kind::Temporary).is_some()
}
pub(crate) fn drop_relations(node: &Node) -> &[Node] {
    if node.kind == Kind::DropView {
        &node.children[2].children
    } else {
        list(node, Kind::TableList)
    }
}
pub(crate) fn fork_source(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn fork_destination(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn rename_actions(node: &Node) -> &[Node] {
    &node.children
}
pub(crate) fn rename_source(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn rename_destination(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn truncate_relation(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn drop_index_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn drop_index_relation(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn alter_table_relation(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_table_actions(node: &Node) -> &[Node] {
    &node.children[1].children
}
pub(crate) fn alter_column_definitions(node: &Node) -> &[Node] {
    match node.kind {
        Kind::ColumnAdd | Kind::ColumnModify => &node.children,
        Kind::ColumnChange => &node.children[1..],
        _ => unreachable!(),
    }
}
pub(crate) fn alter_column_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_column_new_name(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn alter_column_has_default(node: &Node) -> bool {
    node.children[1].kind == Kind::ColumnDefault
}
pub(crate) fn alter_column_default(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn alter_column_drop_behavior(node: &Node) -> DropBehavior {
    match node.value {
        0 => DropBehavior::Default,
        1 => DropBehavior::Restrict,
        2 => DropBehavior::Cascade,
        _ => unreachable!(),
    }
}
pub(crate) fn alter_constraint_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_partition_names(node: &Node) -> &[Node] {
    &node.children[0].children
}
pub(crate) fn alter_partition_definition(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_partition_additions(node: &Node) -> &[Node] {
    &node.children
}
pub(crate) fn alter_partition_exchange_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_partition_exchange_table(node: &Node) -> &Node {
    &node.children[1]
}
pub(crate) fn alter_table_renamed_relation(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_charset_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_charset_has_collation(node: &Node) -> bool {
    node.children[1].kind != Kind::Missing
}
pub(crate) fn alter_charset_collation(node: &Node) -> &Node {
    &node.children[1]
}

pub(crate) fn optimize_relations(node: &Node) -> &[Node] {
    &node.children[0].children
}
pub(crate) fn analyze_relations(node: &Node) -> &[Node] {
    &node.children[0].children
}
pub(crate) fn purge_relation(node: &Node) -> &Node {
    &node.children[0]
}

pub(crate) fn partition_element_values(node: &Node) -> &Node {
    &node.children[1]
}

pub(crate) fn alter_table_options(node: &Node) -> &[Node] {
    &node.children
}
pub(crate) fn alter_index_name(node: &Node) -> &Node {
    &node.children[0]
}
pub(crate) fn alter_index_new_name(node: &Node) -> &Node {
    &node.children[1]
}

pub(crate) fn index_column_has_direction(node: &Node) -> bool {
    node.children[2].kind != Kind::Missing
}
pub(crate) fn primary_key_has_comment(node: &Node) -> bool {
    node.children
        .get(2)
        .is_some_and(|value| value.kind != Kind::Missing)
}
pub(crate) fn primary_key_comment(node: &Node) -> &Node {
    &node.children[2]
}
pub(crate) fn index_hints(node: &Node) -> &[Node] {
    list(node, Kind::Hints)
}
