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

#define USING_LOG_PREFIX SQL_PARSER
#include "rust/sql_parser.h"
#include "sql/parser/rust_sql_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/parse_malloc.h"
#include "lib/string/ob_string.h"
#include "share/ob_errno.h"
#include "share/cache/ob_cache_type.h"
#include "share/schema/ob_priv_type.h"
#include "lib/oblog/ob_log.h"
#include <algorithm>

namespace oceanbase::sql {
using common::OB_ERR_PARSE_SQL;
namespace {
using Kind = peg::NodeKind;

ObItemType operator_type(const peg::Node &node)
{
  const common::ObString op(node.text.size(), node.text.data());
  if (node.kind == Kind::Unary) {
    if (op == "+") return T_OP_POS;
    if (op == "-") return T_OP_NEG;
    if (op == "~") return T_OP_BIT_NEG;
    if (op == "!" || op == "not") return T_OP_NOT;
  } else {
    if (op == "+") return T_OP_ADD;
    if (op == "-") return T_OP_MINUS;
    if (op == "*") return T_OP_MUL;
    if (op == "/") return T_OP_DIV;
    if (op == "%" || op == "mod") return T_OP_MOD;
    if (op == "div") return T_OP_INT_DIV;
    if (op == "=") return T_OP_EQ;
    if (op == "<=>") return T_OP_NSEQ;
    if (op == "!=" || op == "<>") return T_OP_NE;
    if (op == "<") return T_OP_LT;
    if (op == "<=") return T_OP_LE;
    if (op == ">") return T_OP_GT;
    if (op == ">=") return T_OP_GE;
    if (op == "and") return T_OP_AND;
    if (op == "or") return T_OP_OR;
    if (op == "xor") return T_OP_XOR;
    if (op == "&") return T_OP_BIT_AND;
    if (op == "|") return T_OP_BIT_OR;
    if (op == "^") return T_OP_BIT_XOR;
    if (op == "<<") return T_OP_BIT_LEFT_SHIFT;
    if (op == ">>") return T_OP_BIT_RIGHT_SHIFT;
    if (op == "concat") return T_OP_CNN;
    if (op == "like" || op == "not like") return T_OP_LIKE;
  }
  return T_INVALID;
}

ObItemType node_type(const peg::Node &node)
{
  switch (node.kind) {
    case Kind::EmptyQuery: return T_EMPTY_QUERY;
    case Kind::Analyze: return T_MYSQL_ANALYZE;
    case Kind::DbmsStats: return T_DBMS_STATS;
    case Kind::FullScan: return T_FULL_HINT;
    case Kind::IndexScan: return T_INDEX_HINT;
    case Kind::IndexAsc: return T_INDEX_ASC_HINT;
    case Kind::IndexDesc: return T_INDEX_DESC_HINT;
    case Kind::ApproxCountDistinct: return T_FUN_APPROX_COUNT_DISTINCT;
    case Kind::ApproxCountDistinctSynopsis: return T_FUN_APPROX_COUNT_DISTINCT_SYNOPSIS;
    case Kind::ApproxCountDistinctSynopsisMerge: return T_FUN_APPROX_COUNT_DISTINCT_SYNOPSIS_MERGE;
    case Kind::SumOpnsize: return T_FUN_SUM_OPNSIZE;
    case Kind::TopFreHist: return T_FUN_TOP_FRE_HIST;
    case Kind::HybridHist: return T_FUN_HYBRID_HIST;
    case Kind::Select: return T_SELECT;
    case Kind::Union: return T_SET_UNION;
    case Kind::UnionAll: return T_SET_UNION_ALL;
    case Kind::Intersect: return T_SET_INTERSECT;
    case Kind::Except: return T_SET_EXCEPT;
    case Kind::Explain: return T_EXPLAIN;
    case Kind::HashAggregate: return T_USE_HASH_AGGREGATE;
    case Kind::NoHashAggregate: return T_NO_USE_HASH_AGGREGATE;
    case Kind::Parallel: return T_PARALLEL;
    case Kind::TableParallel: return T_TABLE_PARALLEL;
    case Kind::DmlParallel: return T_DML_PARALLEL;
    case Kind::NoParallel: return T_NO_PARALLEL;
    case Kind::EnableParallelDml: return T_ENABLE_PARALLEL_DML;
    case Kind::DisableParallelDml: return T_DISABLE_PARALLEL_DML;
    case Kind::QueryTimeout: return T_QUERY_TIMEOUT;
    case Kind::DynamicSampling: return T_DYNAMIC_SAMPLING;
    case Kind::TableDynamicSampling: return T_TABLE_DYNAMIC_SAMPLING;
    case Kind::Snapshot: return T_TABLE_SNAPSHOT_QUERY_SCN;
    case Kind::TableOptions: return T_TABLE_OPTION_LIST;
    case Kind::TableCharset: return T_CHARSET;
    case Kind::TableCollation: return T_COLLATION;
    case Kind::Compression: return T_COMPRESSION;
    case Kind::TableAutoIncrement: return T_AUTO_INCREMENT;
    case Kind::BlockSize: return T_BLOCK_SIZE;
    case Kind::Index: return T_INDEX;
    case Kind::IndexColumns: return T_INDEX_COLUMN_LIST;
    case Kind::IndexColumn: return T_SORT_COLUMN_KEY;
    case Kind::IndexScope: return T_INDEX_SCOPE;
    case Kind::StoringColumns: return T_STORING_COLUMN_LIST;
    case Kind::UsingHash: return T_USING_HASH;
    case Kind::CreateIndex: return T_CREATE_INDEX;
    case Kind::DropIndex: return T_DROP_INDEX;
    case Kind::PartitionOption: return T_PARTITION_OPTION;
    case Kind::KeyPartition: return T_KEY_PARTITION;
    case Kind::HashPartition: return T_HASH_PARTITION;
    case Kind::PartitionKeyFunction: return T_FUN_SYS_PART_KEY;
    case Kind::PartitionHashFunction: return T_FUN_SYS_PART_HASH;
    case Kind::ProjectList: return T_PROJECT_LIST;
    case Kind::Projection: return T_PROJECT_STRING;
    case Kind::Alias:
    case Kind::StringAlias: return T_ALIAS;
    case Kind::FromList: return T_FROM_LIST;
    case Kind::Relation: return T_RELATION_FACTOR;
    case Kind::Table: return T_ORG;
    case Kind::Ident: return T_IDENT;
    case Kind::Column: return T_COLUMN_REF;
    case Kind::Star: return T_STAR;
    case Kind::Int: return T_INT;
    case Kind::Uint: return T_UINT64;
    case Kind::Number: return T_NUMBER;
    case Kind::Double: return T_DOUBLE;
    case Kind::String: return T_VARCHAR;
    case Kind::Bool: return T_BOOL;
    case Kind::Null: return T_NULL;
    case Kind::Parameter: return T_QUESTIONMARK;
    case Kind::Unary:
    case Kind::Binary: return operator_type(node);
    case Kind::Where: return T_WHERE_CLAUSE;
    case Kind::Order: return T_ORDER_BY;
    case Kind::SortList: return T_SORT_LIST;
    case Kind::SortKey: return T_SORT_KEY;
    case Kind::Asc: return T_SORT_ASC;
    case Kind::Desc: return T_SORT_DESC;
    case Kind::Limit: return T_LIMIT_CLAUSE;
    case Kind::CommaLimit: return T_COMMA_LIMIT_CLAUSE;
    case Kind::Distinct: return T_DISTINCT;
    case Kind::ExprList: return T_EXPR_LIST;
    case Kind::Between: return node.value ? T_OP_NOT_BTW : T_OP_BTW;
    case Kind::In: return node.value ? T_OP_NOT_IN : T_OP_IN;
    case Kind::Is: return node.value ? T_OP_IS_NOT : T_OP_IS;
    case Kind::Function: return T_FUN_SYS;
    case Kind::If: return T_FUN_SYS_IF;
    case Kind::IsNull: return T_FUN_SYS_ISNULL;
    case Kind::CurrentTimestamp: return T_FUN_SYS_CUR_TIMESTAMP;
    case Kind::CurrentTime: return T_FUN_SYS_CUR_TIME;
    case Kind::CurrentDate: return T_FUN_SYS_CUR_DATE;
    case Kind::UtcTimestamp: return T_FUN_SYS_UTC_TIMESTAMP;
    case Kind::UtcTime: return T_FUN_SYS_UTC_TIME;
    case Kind::UtcDate: return T_FUN_SYS_UTC_DATE;
    case Kind::Sysdate: return T_FUN_SYS_SYSDATE;
    case Kind::CurrentUser: return T_FUN_SYS_CURRENT_USER;
    case Kind::Count: return T_FUN_COUNT;
    case Kind::Sum: return T_FUN_SUM;
    case Kind::Min: return T_FUN_MIN;
    case Kind::Max: return T_FUN_MAX;
    case Kind::Avg: return T_FUN_AVG;
    case Kind::All: return T_ALL;
    case Kind::Exists: return T_OP_EXISTS;
    case Kind::JoinedTable: return T_JOINED_TABLE;
    case Kind::InnerJoin: return T_JOIN_INNER;
    case Kind::LeftJoin: return T_JOIN_LEFT;
    case Kind::RightJoin: return T_JOIN_RIGHT;
    case Kind::ColumnList: return T_COLUMN_LIST;
    case Kind::GroupBy: return T_GROUPBY_CLAUSE;
    case Kind::WithRollup: return T_WITH_ROLLUP_CLAUSE;
    case Kind::Insert: return T_INSERT;
    case Kind::Replace: return T_REPLACE;
    case Kind::SingleTableInsert: return T_SINGLE_TABLE_INSERT;
    case Kind::InsertInto: return T_INSERT_INTO_CLAUSE;
    case Kind::ValueList: return T_VALUE_LIST;
    case Kind::ValueVector: return T_VALUE_VECTOR;
    case Kind::Empty: return T_EMPTY;
    case Kind::Default: return T_DEFAULT;
    case Kind::Ignore: return T_IGNORE;
    case Kind::Assignment: return T_ASSIGN_ITEM;
    case Kind::AssignList: return T_ASSIGN_LIST;
    case Kind::TableReferences: return T_TABLE_REFERENCES;
    case Kind::Update: return T_UPDATE;
    case Kind::Delete: return T_DELETE;
    case Kind::DeleteTables: return T_DELETE_TABLE_NODE;
    case Kind::Begin: return T_BEGIN;
    case Kind::Commit: return T_COMMIT;
    case Kind::Rollback: return T_ROLLBACK;
    case Kind::UseDatabase: return T_USE_DATABASE;
    case Kind::CreateDatabase: return T_CREATE_DATABASE;
    case Kind::DropDatabase: return T_DROP_DATABASE;
    case Kind::IfExists: return T_IF_EXISTS;
    case Kind::IfNotExists: return T_IF_NOT_EXISTS;
    case Kind::CreateTable: return T_CREATE_TABLE;
    case Kind::DropTable: return T_DROP_TABLE;
    case Kind::TableList: return T_TABLE_LIST;
    case Kind::TableElements: return T_TABLE_ELEMENT_LIST;
    case Kind::ColumnDefinition: return T_COLUMN_DEFINITION;
    case Kind::ColumnAttributes: return T_COLUMN_ATTRIBUTES;
    case Kind::NotNull: return T_CONSTR_NOT_NULL;
    case Kind::Nullable: return T_CONSTR_NULL;
    case Kind::PrimaryKey: return T_PRIMARY_KEY;
    case Kind::ColumnPrimaryKey: return T_CONSTR_PRIMARY_KEY;
    case Kind::UniqueKey: return T_CONSTR_UNIQUE_KEY;
    case Kind::AutoIncrement: return T_CONSTR_AUTO_INCREMENT;
    case Kind::ColumnDefault: return T_CONSTR_DEFAULT;
    case Kind::ColumnComment: return T_COMMENT;
    case Kind::SystemVariable: return T_SYSTEM_VARIABLE;
    case Kind::UserVariable: return T_USER_VARIABLE_IDENTIFIER;
    case Kind::GetUserVariable: return T_OP_GET_USER_VAR;
    case Kind::VariableSet: return T_VARIABLE_SET;
    case Kind::VariableValue: return T_VAR_VAL;
    case Kind::SetNames: return T_SET_NAMES;
    case Kind::SetCharset: return T_SET_CHARSET;
    case Kind::ShowDatabases: return T_SHOW_DATABASES;
    case Kind::ShowTables: return T_SHOW_TABLES;
    case Kind::ShowColumns: return T_SHOW_COLUMNS;
    case Kind::ShowVariables: return T_SHOW_VARIABLES;
    case Kind::ShowCreateTable: return T_SHOW_CREATE_TABLE;
    case Kind::ShowCreateDatabase: return T_SHOW_CREATE_DATABASE;
    case Kind::ShowWarnings: return T_SHOW_WARNINGS;
    case Kind::ShowErrors: return T_SHOW_ERRORS;
    case Kind::ShowLimit: return T_SHOW_LIMIT;
    case Kind::LikeClause: return T_LIKE_CLAUSE;
    case Kind::Case: return T_CASE;
    case Kind::WhenList: return T_WHEN_LIST;
    case Kind::When: return T_WHEN;
    case Kind::DefaultNull: return T_DEFAULT_NULL;
    case Kind::Savepoint: return T_CREATE_SAVEPOINT;
    case Kind::RollbackSavepoint: return T_ROLLBACK_SAVEPOINT;
    case Kind::ReleaseSavepoint: return T_RELEASE_SAVEPOINT;
    case Kind::LockWait: return T_SFU_INT;
    case Kind::LockWaitDecimal: return T_SFU_DECIMAL;
    case Kind::SkipLocked: return T_SKIP_LOCKED;
    case Kind::HexString:
    case Kind::BitString: return T_HEX_STRING;
    case Kind::SetConfig: return T_ALTER_SYSTEM_SET_PARAMETER;
    case Kind::ResetConfig: return T_ALTER_SYSTEM_RESET_PARAMETER;
    case Kind::FlushCache: return T_FLUSH_CACHE;
    case Kind::CacheType: return T_INT;
    case Kind::CacheNamespace: return T_NAMESPACE;
    case Kind::SqlId: return T_SQL_ID;
    case Kind::SchemaId: return T_SCHEMA_ID;
    case Kind::DatabaseList: return T_DATABASE_LIST;
    case Kind::ConfigList: return T_SYTEM_ACTION_LIST;
    case Kind::ConfigAction: return T_SYSTEM_ACTION;
    case Kind::SetTracepoint: return T_ALTER_SYSTEM_SETTP;
    case Kind::TracepointOptions: return T_SYTEM_SETTP_LIST;
    case Kind::TracepointNumber: return T_TP_NO;
    case Kind::TracepointName: return T_TP_NAME;
    case Kind::TracepointOccurrence: return T_OCCUR;
    case Kind::TracepointFrequency: return T_TRIGGER_MODE;
    case Kind::TracepointError: return T_ERROR_CODE;
    case Kind::TracepointMatch: return T_TP_COND;
    case Kind::Link: return T_LINK_NODE;
    case Kind::Values: return T_VALUES_TABLE_EXPRESSION;
    case Kind::ValuesRows: return T_VALUES_ROW_LIST;
    case Kind::Hints: return T_HINT_OPTION_LIST;
    case Kind::HintRelation: return T_RELATION_FACTOR_IN_HINT;
    case Kind::HintTables: return T_RELATION_FACTOR_IN_HINT_LIST;
    case Kind::JoinHintTables: return T_RELATION_FACTOR_IN_USE_JOIN_HINT_LIST;
    case Kind::Leading: return T_LEADING;
    case Kind::UseNl: return T_USE_NL;
    case Kind::NoUseNl: return T_NO_USE_NL;
    case Kind::UseHash: return T_USE_HASH;
    case Kind::NoUseHash: return T_NO_USE_HASH;
    case Kind::UseMerge: return T_USE_MERGE;
    case Kind::NoUseMerge: return T_NO_USE_MERGE;
    case Kind::QbName: return T_QB_NAME;
    case Kind::OptParam: return T_OPT_PARAM_HINT;
    case Kind::NoRewrite: return T_NO_REWRITE;
    case Kind::UsePlanCache: return T_USE_PLAN_CACHE;
    case Kind::WindowFunction: return T_WINDOW_FUNCTION;
    case Kind::Window: return T_WIN_NEW_GENERALIZED_WINDOW;
    case Kind::WindowSpec: return T_WIN_GENERALIZED_WINDOW;
    case Kind::WindowFrame: return T_WIN_WINDOW;
    case Kind::WindowBound: return T_WIN_BOUND;
    case Kind::WindowInterval: return T_WIN_INTERVAL;
    case Kind::HiddenInt: return T_INT;
    case Kind::RowNumber: return T_WIN_FUN_ROW_NUMBER;
    case Kind::Rank: return T_WIN_FUN_RANK;
    case Kind::DenseRank: return T_WIN_FUN_DENSE_RANK;
    case Kind::PercentRank: return T_WIN_FUN_PERCENT_RANK;
    case Kind::CumeDist: return T_WIN_FUN_CUME_DIST;
    case Kind::Ntile: return T_WIN_FUN_NTILE;
    case Kind::NamedWindows: return T_WIN_NAMED_WINDOWS;
    case Kind::NamedWindow: return T_WIN_NAMED_WINDOW;
    case Kind::CreateUser: return T_CREATE_USER;
    case Kind::DropUser: return T_DROP_USER;
    case Kind::Users: return T_USERS;
    case Kind::UserSpec: return T_CREATE_USER_SPEC;
    case Kind::UserHost: return T_USER_WITH_HOST_NAME;
    case Kind::Password: return T_VARCHAR;
    case Kind::SetPassword: return T_SET_PASSWORD;
    case Kind::Privilege: return T_PRIV_TYPE;
    case Kind::Privileges: return T_PRIVILEGES;
    case Kind::PrivilegeLevel: return T_PRIV_LEVEL;
    case Kind::PrivilegeObject: return T_PRIV_OBJECT;
    case Kind::Grant: return T_GRANT;
    case Kind::Revoke: return T_REVOKE;
    case Kind::RevokeAll: return T_REVOKE_ALL;
    case Kind::ShowGrants: return T_SHOW_GRANTS;
    case Kind::Prepare: return T_PREPARE;
    case Kind::Execute: return T_EXECUTE;
    case Kind::ExecuteArguments: return T_ARGUMENT_LIST;
    case Kind::Deallocate: return T_DEALLOCATE;
    case Kind::CreateProcedure: return T_SP_CREATE;
    case Kind::CreateFunction: return T_SF_CREATE;
    case Kind::DropProcedure: return T_SP_DROP;
    case Kind::DropFunction: return T_SF_DROP;
    case Kind::AlterProcedure: return T_SP_ALTER;
    case Kind::AlterFunction: return T_SF_ALTER;
    case Kind::RoutineName: return T_SP_NAME;
    case Kind::RoutineParameters: return T_SP_PARAM_LIST;
    case Kind::RoutineParameter: return T_SP_PARAM;
    case Kind::RoutineClauses: return T_SP_CLAUSE_LIST;
    case Kind::Deterministic: return T_SP_DETERMINISTIC;
    case Kind::DataAccess: return T_SP_DATA_ACCESS;
    case Kind::InvokeRights: return T_SP_INVOKE;
    case Kind::PlBlock: return T_SP_BLOCK_CONTENT;
    case Kind::PlStatements: return T_SP_PROC_STMT_LIST;
    case Kind::PlDeclarations: return T_SP_DECL_LIST;
    case Kind::PlDeclaration: return T_SP_DECL;
    case Kind::PlIdentifiers: return T_SP_DECL_IDENT_LIST;
    case Kind::PlDefault: return T_SP_DECL_DEFAULT;
    case Kind::PlHandler: return T_SP_DECL_HANDLER;
    case Kind::PlHandlerConditions: return T_SP_HCOND_LIST;
    case Kind::PlConditionDeclaration: return T_SP_DECL_COND;
    case Kind::PlCondition: return T_SP_CONDITION;
    case Kind::SqlState: return T_SQL_STATE;
    case Kind::SqlException: return T_SQL_EXCEPTION;
    case Kind::SqlWarning: return T_SQL_WARNING;
    case Kind::SqlNotFound: return T_SQL_NOT_FOUND;
    case Kind::PlSql: return T_SQL_STMT;
    case Kind::PlSubquery: return T_SELECT;
    case Kind::PlIf: return T_SP_IF;
    case Kind::PlElse: return T_SP_ELSE;
    case Kind::PlCase: return T_SP_CASE;
    case Kind::PlWhen: return T_SP_WHEN;
    case Kind::PlWhile: return T_SP_WHILE;
    case Kind::PlLoop: return T_SP_LOOP;
    case Kind::PlRepeat: return T_SP_REPEAT;
    case Kind::PlReturn: return T_SP_RETURN;
    case Kind::PlLeave: return T_SP_LEAVE;
    case Kind::PlIterate: return T_SP_ITERATE;
    case Kind::PlLabeledBlock: return T_SP_LABELED_BLOCK;
    case Kind::PlLabeledControl: return T_SP_LABELED_CONTROL;
    case Kind::PlCall: return T_SP_CALL_STMT;
    case Kind::PlCallName: return T_SP_ACCESS_NAME;
    case Kind::PlCallArguments: return T_SP_CPARAM_LIST;
    case Kind::Diagnostics: return T_DIAGNOSTICS;
    case Kind::PlSignal: return T_SP_SIGNAL;
    case Kind::PlResignal: return T_SP_RESIGNAL;
    case Kind::PlSignalItems: return T_SP_SIGNAL_INFO_LIST;
    case Kind::PlSignalItem: return T_SP_SIGNAL_INFO_ITEM;
    case Kind::PlCursor: return T_SP_DECL_CURSOR;
    case Kind::PlOpen: return T_SP_PROC_OPEN;
    case Kind::PlClose: return T_SP_PROC_CLOSE;
    case Kind::PlFetch: return T_SP_PROC_FETCH;
    case Kind::PlInto: return T_SP_INTO_LIST;
    case Kind::IntoVariables: return T_INTO_VARIABLES;
    case Kind::IntoVariableList: return T_INTO_VARS_LIST;
    case Kind::CreatePackage: return T_PACKAGE_CREATE;
    case Kind::CreatePackageBody: return T_PACKAGE_CREATE_BODY;
    case Kind::DropPackage: return T_PACKAGE_DROP;
    case Kind::Package: return T_PACKAGE_BLOCK;
    case Kind::PackageBody: return T_PACKAGE_BODY_BLOCK;
    case Kind::PackageStatements: return T_PACKAGE_STMTS;
    case Kind::PackageBodyStatements: return T_PACKAGE_BODY_STMTS;
    case Kind::SubProcedureDeclaration: return T_SUB_PROC_DECL;
    case Kind::SubFunctionDeclaration: return T_SUB_FUNC_DECL;
    case Kind::SubProcedureDefinition: return T_SUB_PROC_DEF;
    case Kind::SubFunctionDefinition: return T_SUB_FUNC_DEF;
    case Kind::InterfacePragma: return T_SP_PRAGMA_INTERFACE;
    case Kind::ObjectAccess: return T_OBJ_ACCESS_REF;
    case Kind::JsonTable: return T_JSON_TABLE_EXPRESSION;
    case Kind::JsonColumn: return T_JSON_TABLE_COLUMN;
    case Kind::HiddenNull: return T_NULL;
    case Kind::NullClause: return T_NULLX_CLAUSE;
    default: return T_INVALID;
  }
}

ObPrivType privilege_type(const peg::Node &node)
{
  if (node.text == "all") return OB_PRIV_ALL;
  if (node.text == "alter") return OB_PRIV_ALTER;
  if (node.text == "create") return OB_PRIV_CREATE;
  if (node.text == "create user") return OB_PRIV_CREATE_USER;
  if (node.text == "delete") return OB_PRIV_DELETE;
  if (node.text == "drop") return OB_PRIV_DROP;
  if (node.text == "grant option") return OB_PRIV_GRANT;
  if (node.text == "insert") return OB_PRIV_INSERT;
  if (node.text == "update") return OB_PRIV_UPDATE;
  if (node.text == "select") return OB_PRIV_SELECT;
  if (node.text == "index") return OB_PRIV_INDEX;
  if (node.text == "create view") return OB_PRIV_CREATE_VIEW;
  if (node.text == "show view") return OB_PRIV_SHOW_VIEW;
  if (node.text == "show databases") return OB_PRIV_SHOW_DB;
  if (node.text == "super") return OB_PRIV_SUPER;
  if (node.text == "process") return OB_PRIV_PROCESS;
  if (node.text == "file") return OB_PRIV_FILE;
  if (node.text == "replication slave") return OB_PRIV_REPL_SLAVE;
  if (node.text == "replication client") return OB_PRIV_REPL_CLIENT;
  if (node.text == "alter system") return OB_PRIV_ALTER_SYSTEM;
  if (node.text == "execute") return OB_PRIV_EXECUTE;
  if (node.text == "alter routine") return OB_PRIV_ALTER_ROUTINE;
  if (node.text == "create routine") return OB_PRIV_CREATE_ROUTINE;
  if (node.text == "shutdown") return OB_PRIV_SHUTDOWN;
  if (node.text == "reload") return OB_PRIV_RELOAD;
  if (node.text == "lock tables") return OB_PRIV_LOCK_TABLE;
  if (node.text == "references") return OB_PRIV_REFERENCES;
  if (node.text == "create role") return OB_PRIV_CREATE_ROLE;
  if (node.text == "drop role") return OB_PRIV_DROP_ROLE;
  if (node.text == "trigger") return OB_PRIV_TRIGGER;
  if (node.text == "read") return OB_PRIV_READ;
  if (node.text == "write") return OB_PRIV_WRITE;
  if (node.text == "create ai model") return OB_PRIV_CREATE_AI_MODEL;
  if (node.text == "alter ai model") return OB_PRIV_ALTER_AI_MODEL;
  if (node.text == "drop ai model") return OB_PRIV_DROP_AI_MODEL;
  if (node.text == "access ai model") return OB_PRIV_ACCESS_AI_MODEL;
  if (node.text == "usage") return OB_PRIV_SET_EMPTY;
  return -1;
}

int build_type(const peg::Node &source, void *pool, ParseNode *&node)
{
  const common::ObString name(source.text.size(), source.text.data());
  ObItemType type = T_INVALID;
  if (name == "tinyint" || name == "bool" || name == "boolean") type = T_TINYINT;
  else if (name == "smallint") type = T_SMALLINT;
  else if (name == "mediumint") type = T_MEDIUMINT;
  else if (name == "int") type = T_INT32;
  else if (name == "bigint" || name == "signed") type = T_INT;
  else if (name == "unsigned") type = T_UINT64;
  else if (name == "float") type = T_FLOAT;
  else if (name == "double") type = T_DOUBLE;
  else if (name == "decimal") type = T_NUMBER;
  else if (name == "varchar" || name == "varbinary") type = T_VARCHAR;
  else if (name == "char" || name == "binary") type = T_CHAR;
  else if (name == "text" || name == "blob") type = T_TEXT;
  else if (name == "longtext" || name == "longblob") type = T_LONGTEXT;
  else if (name == "datetime") type = T_DATETIME;
  else if (name == "timestamp") type = T_TIMESTAMP;
  else if (name == "time") type = T_TIME;
  else if (name == "date") type = T_DATE;
  else if (name == "json") type = T_JSON;
  else if (name == "year") type = T_YEAR;
  if (type == T_INVALID) return common::OB_ERR_UNEXPECTED;
  const bool integer = type == T_TINYINT || type == T_SMALLINT || type == T_MEDIUMINT
      || type == T_INT32 || type == T_INT;
  const bool string = type == T_CHAR || type == T_VARCHAR || type == T_TEXT || type == T_LONGTEXT;
  const bool binary = name == "binary" || name == "varbinary" || name == "blob" || name == "longblob";
  if (source.value != 0) {
    if (integer) type = static_cast<ObItemType>(type + T_UTINYINT - T_TINYINT);
    else if (type == T_FLOAT || type == T_DOUBLE) type = static_cast<ObItemType>(type + T_UFLOAT - T_FLOAT);
    else if (type == T_NUMBER) type = T_UNUMBER;
  }
  const bool cast = source.kind == Kind::CastType;
  node = new_node(pool, cast ? T_CAST_ARGUMENT : type, !cast && string && !binary ? 3 : 0);
  if (node == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  node->value_ = 0;
  const auto &args = source.children;
  const int32_t precision = args.empty() ? 0 : std::min<uint64_t>(args[0].value, INT32_MAX);
  const int16_t short_precision = std::min<int32_t>(precision, INT16_MAX);
  const int16_t scale = args.size() < 2 ? 0 : std::min<uint64_t>(args[1].value, INT16_MAX);
  if (cast) {
    node->int16_values_[OB_NODE_CAST_TYPE_IDX] = type;
    node->param_num_ = args.size();
    if (type == T_CHAR || type == T_VARCHAR) {
      node->int16_values_[OB_NODE_CAST_COLL_IDX] = binary ? 63 : 0;
      node->int32_values_[OB_NODE_CAST_C_LEN_IDX] = args.empty() ? -1 : precision;
    } else if (type == T_FLOAT || type == T_DOUBLE || type == T_NUMBER) {
      node->int16_values_[OB_NODE_CAST_N_PREC_IDX] = args.empty()
          ? (type == T_NUMBER ? 10 : type == T_FLOAT ? 0 : -1) : short_precision;
      node->int16_values_[OB_NODE_CAST_N_SCALE_IDX] = type == T_NUMBER ? scale : -1;
    } else if (type == T_DATETIME || type == T_DATE || type == T_TIME || type == T_YEAR) {
      node->int16_values_[OB_NODE_CAST_N_SCALE_IDX] = short_precision;
    }
  } else if (string) {
    node->int32_values_[0] = args.empty() && type == T_CHAR ? 1 : precision;
    node->int32_values_[1] = binary;
  } else if (integer) {
    node->int16_values_[0] = args.empty() ? -1 : short_precision;
    if (name == "bool" || name == "boolean") {
      node->int16_values_[0] = 1;
      node->int16_values_[1] = name == "bool" ? 1 : 2;
    }
  } else if (type == T_DATETIME || type == T_TIMESTAMP || type == T_TIME) {
    node->int16_values_[1] = short_precision;
  } else if (name == "decimal" || name == "float" || name == "double") {
    node->int16_values_[0] = args.empty() ? (name == "decimal" ? 10 : -1) : short_precision;
    node->int16_values_[1] = name != "decimal" && args.size() < 2 ? -1 : scale;
  }
  if (!cast && !string) {
    node->int16_values_[2] = (source.value & 2) != 0;
    node->int16_values_[3] = (source.value & 1) != 0;
  }
  node->sql_str_off_ = source.start;
  return common::OB_SUCCESS;
}

int build_node(const common::ObString &sql, const peg::Node &source, size_t parameters,
               void *pool, ParseNode *&node, bool pl = false)
{
  node = nullptr;
  if (source.kind == Kind::Missing) return common::OB_SUCCESS;
  if (source.kind == Kind::DataType || source.kind == Kind::CastType) return build_type(source, pool, node);
  if (source.kind == Kind::Parenthesized) {
    const int ret = build_node(sql, source.children[0], parameters, pool, node, pl);
    if (ret != common::OB_SUCCESS) return ret;
    node->is_assigned_from_child_ = true;
    return common::OB_SUCCESS;
  }
  const ObItemType type = node_type(source);
  pl = pl || type == T_SP_CREATE || type == T_SF_CREATE || type == T_SP_CALL_STMT
      || type == T_PACKAGE_CREATE || type == T_PACKAGE_CREATE_BODY;
  if (type == T_INVALID) return common::OB_ERR_UNEXPECTED;
  node = new_node(pool, type, type == T_SELECT ? PARSE_SELECT_MAX_IDX : source.children.size());
  if (node == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  const int select_slots[] = {PARSE_SELECT_DISTINCT, PARSE_SELECT_SELECT, PARSE_SELECT_INTO, PARSE_SELECT_FROM,
                             PARSE_SELECT_WHERE, PARSE_SELECT_DYNAMIC_GROUP, PARSE_SELECT_DYNAMIC_HAVING,
                             PARSE_SELECT_NAMED_WINDOWS, PARSE_SELECT_SET, PARSE_SELECT_ORDER, PARSE_SELECT_LIMIT,
                             PARSE_SELECT_FOR_UPD, PARSE_SELECT_HINTS, PARSE_SELECT_INTO_EXTRA};
  for (size_t child = 0; child < source.children.size(); ++child) {
    const size_t slot = type == T_SELECT ? select_slots[child] : child;
    const int ret = build_node(sql, source.children[child], parameters, pool, node->children_[slot], pl);
    if (ret != common::OB_SUCCESS) return ret;
  }
  if (type == T_SELECT && node->children_[PARSE_SELECT_DISTINCT] != nullptr) {
    ParseNode *options = new_node(pool, T_QEURY_EXPRESSION_LIST, 1);
    if (options == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    options->children_[0] = node->children_[PARSE_SELECT_DISTINCT];
    node->children_[PARSE_SELECT_DISTINCT] = options;
  }
  if (type != T_FUN_SYS) node->value_ = type == T_SELECT ? parameters : source.value;
  node->sql_str_off_ = source.start;
  node->raw_sql_offset_ = source.start;
  node->stmt_loc_.first_column_ = source.start + 1;
  node->stmt_loc_.last_column_ = source.end;
  if (pl) {
    node->pl_str_off_ = source.start;
    node->stmt_loc_.first_column_ = source.start;
    node->stmt_loc_.last_column_ = source.end == 0 ? 0 : source.end - 1;
  }
  const char *raw = sql.ptr() + source.start;
  const size_t raw_length = source.end - source.start;
  const bool literal = type == T_INT || type == T_UINT64 || type == T_NUMBER || type == T_DOUBLE
      || type == T_VARCHAR || type == T_HEX_STRING || type == T_BOOL || type == T_NULL || type == T_QUESTIONMARK;
  if (literal && source.kind != Kind::HiddenInt && source.kind != Kind::HiddenNull && source.kind != Kind::CacheType) {
    node->raw_text_ = parse_strndup(raw, raw_length, pool);
    if (node->raw_text_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->text_len_ = raw_length;
    node->param_num_ = 1;
  }
  const char *text = source.text.data();
  size_t length = source.text.size();
  if (type == T_PROJECT_STRING || type == T_COLUMN_REF || type == T_OP_POS || type == T_OP_NEG
      || source.kind == Kind::Binary || source.kind == Kind::Between || source.kind == Kind::In
      || source.kind == Kind::Is || source.kind == Kind::Window || source.kind == Kind::PlSql
      || source.kind == Kind::RoutineParameters || source.kind == Kind::PlDefault
      || source.kind == Kind::SubProcedureDeclaration || source.kind == Kind::SubFunctionDeclaration
      || source.kind == Kind::ObjectAccess || source.kind == Kind::KeyPartition || source.kind == Kind::HashPartition) {
    text = raw;
    length = raw_length;
  }
  if (type == T_HEX_STRING) {
    const bool bits = source.kind == Kind::BitString;
    const size_t size = bits ? ob_parse_bit_string_len(length) : ob_parse_binary_len(length);
    char *bytes = static_cast<char *>(parse_malloc(size + 1, pool));
    if (bytes == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    if (bits) ob_parse_bit_string(text, length, bytes);
    else ob_parse_binary(text, length, bytes);
    bytes[size] = '\0';
    node->str_value_ = bytes;
    node->str_len_ = size;
  } else if (length != 0 || type == T_VARCHAR || type == T_IDENT) {
    node->str_value_ = parse_strndup(text, length, pool);
    if (node->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->str_len_ = length;
  }
  if (source.kind == Kind::PlSubquery) {
    const char *prefix = "(SELECT ";
    const char *suffix = " AS SUBQUERY)";
    const size_t length = strlen(prefix) + raw_length + strlen(suffix);
    char *query = static_cast<char *>(parse_malloc(length + 1, pool));
    if (query == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    snprintf(query, length + 1, "%s%.*s%s", prefix, static_cast<int>(raw_length), raw, suffix);
    node->str_value_ = query;
    node->str_len_ = length;
    node->pl_str_off_ = -1;
  }
  if (source.kind == Kind::Privilege) {
    node->value_ = privilege_type(source);
    if (node->value_ < 0) return common::OB_ERR_UNEXPECTED;
  }
  if (source.kind == Kind::Password) {
    node->stmt_loc_.first_column_ = source.start;
    node->stmt_loc_.last_column_ = source.end - 1;
  }
  if (source.kind == Kind::StringAlias) {
    node->param_num_ = 1;
    node->children_[1]->is_input_quoted_ = false;
  }
  if (source.kind == Kind::HiddenInt || source.kind == Kind::HiddenNull) node->is_hidden_const_ = true;
  if (source.kind == Kind::CacheType) {
    node->value_ = source.text == "plan" ? CACHE_TYPE_PLAN : source.text == "pl" ? CACHE_TYPE_PL_OBJ
        : source.text == "ps" ? CACHE_TYPE_PS_OBJ : CACHE_TYPE_LIB_CACHE;
  }
  if (type == T_BOOL) node->is_literal_bool_ = true;
  if (type == T_SFU_INT && source.start == source.end) node->is_hidden_const_ = true;
  if (type == T_SORT_ASC || type == T_SORT_DESC) node->value_ = 2;
  if (type == T_IDENT) {
    node->is_input_quoted_ = raw_length > 0 && (*raw == '`' || *raw == '"');
  }
  if (type == T_CONSTR_DEFAULT) {
    const auto &value = source.children[0];
    node->is_default_literal_expression_ = value.kind != Kind::Parenthesized;
    node->str_value_ = parse_strndup(sql.ptr() + value.start, value.end - value.start, pool);
    if (node->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->str_len_ = value.end - value.start;
  }
  if (type == T_PROJECT_STRING && node->children_[0]->type_ == T_VARCHAR) {
    node->str_value_ = node->children_[0]->str_value_;
    node->str_len_ = node->children_[0]->str_len_;
  }
  if (type == T_OP_NEG && node->children_[0]->type_ == T_UINT64) {
    ParseNode *child = node->children_[0];
    if (!child->is_assigned_from_child_
        && static_cast<uint64_t>(child->value_) == (uint64_t(1) << 63)) {
      child->type_ = T_INT;
      child->value_ = INT64_MIN;
      child->str_value_ = node->str_value_;
      child->str_len_ = node->str_len_;
      child->raw_text_ = node->str_value_;
      child->text_len_ = node->str_len_;
      child->sql_str_off_ = source.start;
      node = child;
    } else {
      child->type_ = T_NUMBER;
    }
  }
  if (type == T_PACKAGE_BLOCK || type == T_PACKAGE_BODY_BLOCK) {
    const size_t start = source.children[type == T_PACKAGE_BLOCK ? 2 : 1].start;
    node->str_value_ = parse_strndup(sql.ptr() + start, source.end - start, pool);
    if (node->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->str_len_ = source.end - start;
    node->pl_str_off_ = start;
  }
  if (type == T_PACKAGE_CREATE || type == T_PACKAGE_CREATE_BODY) {
    const auto &body_source = source.children[0];
    ParseNode *body = node->children_[0];
    body->str_value_ = parse_strndup(sql.ptr() + body_source.start, body_source.end - body_source.start, pool);
    if (body->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    body->str_len_ = body_source.end - body_source.start;
    body->pl_str_off_ = body_source.start;
  }
  if (type == T_SP_CREATE || type == T_SF_CREATE || type == T_SUB_PROC_DEF || type == T_SUB_FUNC_DEF) {
    ParseNode *body = node->children_[node->num_child_ - 1];
    body->str_value_ = parse_strndup(raw, raw_length, pool);
    if (body->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    body->str_len_ = raw_length;
    const auto &body_source = source.children[source.children.size() - 1];
    body->raw_text_ = body->str_value_ + body_source.start - source.start;
    body->text_len_ = body_source.end - body_source.start;
  }
  if (type == T_OP_LIKE && source.text == "not like") {
    ParseNode *negation = new_node(pool, T_OP_NOT, 1);
    if (negation == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    negation->children_[0] = node;
    node = negation;
  }
  return common::OB_SUCCESS;
}

int build_tree(const common::ObString &sql, peg::ParseTree &tree, ParseResult &result, bool routine_body)
{
  ParseNode *query = nullptr;
  const int ret = build_node(sql, tree.root, tree.parameters, result.malloc_pool_, query, routine_body);
  if (ret != common::OB_SUCCESS) return ret;
  ParseNode *root = new_node(result.malloc_pool_, T_STMT_LIST, 1);
  if (root == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  root->children_[0] = query;
  result.result_tree_ = root;
  result.contain_sensitive_data_ = tree.root.kind == Kind::SetConfig
      || tree.root.kind == Kind::CreateUser || tree.root.kind == Kind::SetPassword || tree.root.kind == Kind::Grant;
  result.may_contain_sensitive_data_ = result.contain_sensitive_data_
      || tree.root.kind == Kind::DropUser || tree.root.kind == Kind::Revoke || tree.root.kind == Kind::RevokeAll;
  if (tree.parameter_names.empty()) {
    result.question_mark_ctx_.count_ = tree.parameters;
    result.question_mark_ctx_.by_ordinal_ = tree.parameters != 0;
  } else {
    for (auto &name : tree.parameter_names) {
      if (get_question_mark(&result.question_mark_ctx_, result.malloc_pool_, name.c_str()) < 0) {
        return common::OB_ALLOCATE_MEMORY_FAILED;
      }
      result.question_mark_ctx_.by_name_ |= name.data()[1] < '0' || name.data()[1] > '9';
    }
  }
  if (result.is_dynamic_sql_) {
    MEMCPY(result.no_param_sql_, sql.ptr(), sql.length());
    result.no_param_sql_[sql.length()] = '\0';
    result.no_param_sql_len_ = sql.length();
  }
  return common::OB_SUCCESS;
}
}

int parse_rust_sql(const common::ObString &sql, ParseResult &result, bool routine_body)
{
  static thread_local char error[MAX_ERROR_MSG];
  error[0] = '\0';
  result.error_msg_ = error;
  result.input_sql_ = sql.ptr();
  result.input_sql_len_ = sql.length();
  auto parsed = peg::parse_sql(rust::Slice<const uint8_t>(
      reinterpret_cast<const uint8_t *>(sql.ptr()), sql.length()),
      result.sql_mode_ & SMO_ANSI_QUOTES, result.sql_mode_ & SMO_NO_BACKSLASH_ESCAPES,
      result.sql_mode_ & SMO_PIPES_AS_CONCAT, result.sql_mode_ & SMO_HIGH_NOT_PRECEDENCE, routine_body);
  if (!parsed.error.empty()) {
    snprintf(error, sizeof(error), "rust-peg: %s", parsed.error.c_str());
    result.start_col_ = parsed.error_offset + 1;
    result.end_col_ = parsed.error_offset + 1;
    int line = 1;
    for (size_t i = 0; i < parsed.error_offset; ++i) line += sql.ptr()[i] == '\n';
    size_t length = std::min<size_t>(80, sql.length() - parsed.error_offset);
    while (length > 0 && parsed.error_offset + length < sql.length()
           && (static_cast<unsigned char>(sql.ptr()[parsed.error_offset + length]) & 0xc0) == 0x80) {
      --length;
    }
    LOG_USER_ERROR(OB_ERR_PARSE_SQL, ob_errpkt_strerror(OB_ERR_PARSER_SYNTAX),
                   static_cast<int>(length), sql.ptr() + parsed.error_offset, line);
    return common::OB_ERR_PARSE_SQL;
  }
  return build_tree(sql, parsed, result, routine_body);
}
}
