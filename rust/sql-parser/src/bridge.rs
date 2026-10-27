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
        Union,
        UnionAll,
        Intersect,
        Except,
        Explain,
        HashAggregate,
        NoHashAggregate,
        Parallel,
        TableParallel,
        DmlParallel,
        NoParallel,
        EnableParallelDml,
        DisableParallelDml,
        QueryTimeout,
        DynamicSampling,
        TableDynamicSampling,
        Snapshot,
        TableOptions,
        TableCharset,
        Collation,
        SetCollation,
        Compression,
        TableAutoIncrement,
        BlockSize,
        Index,
        IndexColumns,
        IndexColumn,
        IndexScope,
        StoringColumns,
        UsingHash,
        CreateIndex,
        DropIndex,
        PartitionOption,
        KeyPartition,
        HashPartition,
        PartitionKeyFunction,
        PartitionHashFunction,
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
        TruncateTable,
        RenameTables,
        RenameTableAction,
        AlterTable,
        AlterTableActions,
        AlterTableOption,
        TableRename,
        AlterColumnOption,
        ColumnAdd,
        ColumnDrop,
        ColumnModify,
        ColumnChange,
        ColumnRename,
        ColumnAlter,
        ColumnFirst,
        ColumnBefore,
        ColumnAfter,
        AlterIndexOption,
        IndexAdd,
        IndexDrop,
        IndexRename,
        PrimaryKeyDrop,
        ShowIndexes,
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
        CreateProcedure,
        CreateFunction,
        DropProcedure,
        DropFunction,
        AlterProcedure,
        AlterFunction,
        RoutineName,
        RoutineParameters,
        RoutineParameter,
        RoutineClauses,
        Deterministic,
        DataAccess,
        InvokeRights,
        PlBlock,
        PlStatements,
        PlDeclarations,
        PlDeclaration,
        PlIdentifiers,
        PlDefault,
        PlHandler,
        PlHandlerConditions,
        PlConditionDeclaration,
        PlCondition,
        SqlState,
        SqlException,
        SqlWarning,
        SqlNotFound,
        PlSql,
        PlSubquery,
        PlIf,
        PlElse,
        PlCase,
        PlWhen,
        PlWhile,
        PlLoop,
        PlRepeat,
        PlReturn,
        PlLeave,
        PlIterate,
        PlLabeledBlock,
        PlLabeledControl,
        PlCall,
        PlCallName,
        PlCallArguments,
        Diagnostics,
        PlSignal,
        PlResignal,
        PlSignalItems,
        PlSignalItem,
        PlCursor,
        PlOpen,
        PlClose,
        PlFetch,
        PlInto,
        IntoVariables,
        IntoVariableList,
        CreatePackage,
        CreatePackageBody,
        DropPackage,
        Package,
        PackageBody,
        PackageStatements,
        PackageBodyStatements,
        SubProcedureDeclaration,
        SubFunctionDeclaration,
        SubProcedureDefinition,
        SubFunctionDefinition,
        InterfacePragma,
        ObjectAccess,
        JsonTable,
        JsonColumn,
        HiddenNull,
        NullClause,
        Analyze,
        DbmsStats,
        FullScan,
        IndexScan,
        IndexAsc,
        IndexDesc,
        ApproxCountDistinct,
        ApproxCountDistinctSynopsis,
        ApproxCountDistinctSynopsisMerge,
        SumOpnsize,
        TopFreHist,
        HybridHist,
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
        parameter_names: Vec<String>,
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
            routine_body: bool,
        ) -> ParseTree;
    }
}

fn number_parameters(
    node: &mut Node,
    count: &mut usize,
    names: &mut Vec<String>,
) -> Result<(), usize> {
    for child in &mut node.children {
        number_parameters(child, count, names)?;
    }
    if node.kind == Kind::Parameter {
        if node.text.is_empty() {
            if !names.is_empty() {
                return Err(node.start);
            }
            node.value = *count as u64;
            *count += 1;
        } else {
            if *count != names.len() {
                return Err(node.start);
            }
            let index = names
                .iter()
                .position(|name| name.eq_ignore_ascii_case(&node.text));
            node.value = index.unwrap_or_else(|| {
                names.push(node.text.clone());
                *count += 1;
                names.len() - 1
            }) as u64;
        }
    }
    Ok(())
}

fn parse_sql(
    sql: &[u8],
    ansi_quotes: bool,
    no_backslash_escapes: bool,
    pipes_as_concat: bool,
    high_not_precedence: bool,
    routine_body: bool,
) -> ffi::ParseTree {
    let mut result = ffi::ParseTree {
        root: Node::missing(),
        parameters: 0,
        parameter_names: Vec::new(),
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
    let parsed = if routine_body {
        grammar::sql::routine_body(sql, mode)
    } else {
        grammar::sql::query(sql, mode)
    };
    match parsed {
        Ok(mut query) => {
            match number_parameters(
                &mut query,
                &mut result.parameters,
                &mut result.parameter_names,
            ) {
                Ok(()) => result.root = query,
                Err(offset) => {
                    result.error_offset = offset;
                    result.error = "ordinal and named parameters cannot be combined".into();
                }
            }
        }
        Err(error) => {
            result.error_offset = error.location.offset;
            result.error = error.to_string();
        }
    }
    result
}
