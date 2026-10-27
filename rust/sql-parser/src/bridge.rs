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
    grammar,
    node::{Kind, Node},
    SqlMode,
};

#[cxx::bridge(namespace = "oceanbase::sql::peg")]
pub(crate) mod ffi {
    enum NodeKind {
        Missing,
        EmptyQuery,
        Select,
        ProjectList,
        Projection,
        Alias,
        StringAlias,
        FromList,
        Relation,
        Table,
        Ident,
        Column,
        Star,
        Int,
        Uint,
        Number,
        Double,
        String,
        Bool,
        Null,
        Parameter,
        Parenthesized,
        Unary,
        Binary,
        Where,
        Order,
        SortList,
        SortKey,
        Asc,
        Desc,
        Limit,
        CommaLimit,
        Distinct,
        ExprList,
        Between,
        In,
        Is,
        Function,
        If,
        IsNull,
        CurrentTimestamp,
        CurrentTime,
        CurrentDate,
        UtcTimestamp,
        UtcTime,
        UtcDate,
        Sysdate,
        CurrentUser,
        Count,
        Sum,
        Min,
        Max,
        Avg,
        All,
        Exists,
        JoinedTable,
        InnerJoin,
        LeftJoin,
        RightJoin,
        ColumnList,
        GroupBy,
        WithRollup,
        Insert,
        Replace,
        SingleTableInsert,
        InsertInto,
        ValueList,
        ValueVector,
        Empty,
        Default,
        Ignore,
        Assignment,
        AssignList,
        TableReferences,
        Update,
        Delete,
        DeleteTables,
        Begin,
        Commit,
        Rollback,
        UseDatabase,
        CreateDatabase,
        DropDatabase,
        IfExists,
        IfNotExists,
        CreateTable,
        DropTable,
        TableList,
        TableElements,
        ColumnDefinition,
        ColumnAttributes,
        NotNull,
        Nullable,
        PrimaryKey,
        ColumnPrimaryKey,
        UniqueKey,
        AutoIncrement,
        ColumnDefault,
        ColumnComment,
        DataType,
        SystemVariable,
        UserVariable,
        GetUserVariable,
        VariableSet,
        VariableValue,
        SetNames,
        SetCharset,
        ShowDatabases,
        ShowTables,
        ShowColumns,
        ShowVariables,
        ShowCreateTable,
        ShowCreateDatabase,
        ShowWarnings,
        ShowErrors,
        ShowLimit,
        LikeClause,
        Case,
        WhenList,
        When,
        DefaultNull,
        Savepoint,
        RollbackSavepoint,
        ReleaseSavepoint,
        LockWait,
        LockWaitDecimal,
        SkipLocked,
        HexString,
        BitString,
        CastType,
        SetConfig,
        ResetConfig,
        FlushCache,
        CacheType,
        CacheNamespace,
        SqlId,
        SchemaId,
        DatabaseList,
        ConfigList,
        ConfigAction,
        SetTracepoint,
        TracepointOptions,
        TracepointNumber,
        TracepointName,
        TracepointOccurrence,
        TracepointFrequency,
        TracepointError,
        TracepointMatch,
        Link,
        Values,
        ValuesRows,
        Hints,
        HintRelation,
        HintTables,
        JoinHintTables,
        Leading,
        UseNl,
        NoUseNl,
        UseHash,
        NoUseHash,
        UseMerge,
        NoUseMerge,
        QbName,
        OptParam,
        NoRewrite,
        UsePlanCache,
        WindowFunction,
        Window,
        WindowSpec,
        WindowFrame,
        WindowBound,
        WindowInterval,
        HiddenInt,
        RowNumber,
        Rank,
        DenseRank,
        PercentRank,
        CumeDist,
        Ntile,
        NamedWindows,
        NamedWindow,
        CreateUser,
        DropUser,
        Users,
        UserSpec,
        UserHost,
        Password,
        SetPassword,
        Privilege,
        Privileges,
        PrivilegeLevel,
        PrivilegeObject,
        Grant,
        Revoke,
        RevokeAll,
        ShowGrants,
        Prepare,
        Execute,
        ExecuteArguments,
        Deallocate,
    }

    struct Node {
        kind: NodeKind,
        children: Vec<Node>,
        text: String,
        start: usize,
        end: usize,
        value: u64,
        depth: usize,
    }

    struct ParseTree {
        root: Node,
        parameters: usize,
        error: String,
        error_offset: usize,
    }

    extern "Rust" {
        fn parse_sql(
            sql: &[u8],
            ansi_quotes: bool,
            no_backslash_escapes: bool,
            pipes_as_concat: bool,
            high_not_precedence: bool,
        ) -> ParseTree;
    }
}

fn number_parameters(node: &mut Node, count: &mut usize) {
    for child in &mut node.children {
        number_parameters(child, count);
    }
    if node.kind == Kind::Parameter {
        node.value = *count as u64;
        *count += 1;
    }
}

fn parse_sql(
    sql: &[u8],
    ansi_quotes: bool,
    no_backslash_escapes: bool,
    pipes_as_concat: bool,
    high_not_precedence: bool,
) -> ffi::ParseTree {
    let mut result = ffi::ParseTree {
        root: Node::missing(),
        parameters: 0,
        error: String::new(),
        error_offset: 0,
    };
    let sql = match std::str::from_utf8(sql) {
        Ok(sql) => sql,
        Err(error) => {
            result.error_offset = error.valid_up_to();
            result.error = error.to_string();
            return result;
        }
    };
    let mode = SqlMode {
        ansi_quotes,
        no_backslash_escapes,
        pipes_as_concat,
        high_not_precedence,
    };
    stacker::maybe_grow(256 * 1024, 2 * 1024 * 1024, || {
        match grammar::sql::query(sql, mode) {
            Ok(mut query) => {
                number_parameters(&mut query, &mut result.parameters);
                result.root = query;
            }
            Err(error) => {
                result.error_offset = error.location.offset;
                result.error = error.to_string();
            }
        }
    });
    result
}
