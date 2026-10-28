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
#include "lib/ob_date_unit_type.h"
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
    if (op == "regexp") return T_OP_REGEXP;
    if (op == "not regexp") return T_OP_NOT_REGEXP;
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
    case Kind::WithClause: return T_WITH_CLAUSE_LIST;
    case Kind::CommonTable: return T_WITH_CLAUSE_AS;
    case Kind::SelectOptions: return T_QEURY_EXPRESSION_LIST;
    case Kind::FoundRows: return T_FOUND_ROWS;
    case Kind::StraightJoin: return T_STRAIGHT_JOIN;
    case Kind::Union: return T_SET_UNION;
    case Kind::UnionAll: return T_SET_UNION_ALL;
    case Kind::Intersect: return T_SET_INTERSECT;
    case Kind::Except: return T_SET_EXCEPT;
    case Kind::Explain: return T_EXPLAIN;
    case Kind::ExplainBasic: return T_BASIC;
    case Kind::ExplainOutline: return T_OUTLINE;
    case Kind::ExplainExtended: return T_EXTENDED;
    case Kind::ExplainExtendedNoAddr: return T_EXTENDED_NOADDR;
    case Kind::ExplainPartitions: return T_PARTITIONS;
    case Kind::ExplainPlanRegress: return T_PLANREGRESS;
    case Kind::ExplainTraditional: return T_TRADITIONAL;
    case Kind::ExplainJson: return T_FORMAT_JSON;
    case Kind::ExplainPretty: return T_PRETTY;
    case Kind::ExplainPrettyColor: return T_PRETTY_COLOR;
    case Kind::HashAggregate: return T_USE_HASH_AGGREGATE;
    case Kind::NoHashAggregate: return T_NO_USE_HASH_AGGREGATE;
    case Kind::Parallel: return T_PARALLEL;
    case Kind::TableParallel: return T_TABLE_PARALLEL;
    case Kind::DmlParallel: return T_DML_PARALLEL;
    case Kind::NoParallel: return T_NO_PARALLEL;
    case Kind::EnableParallelDml: return T_ENABLE_PARALLEL_DML;
    case Kind::DisableParallelDml: return T_DISABLE_PARALLEL_DML;
    case Kind::QueryTimeout: return T_QUERY_TIMEOUT;
    case Kind::TopK: return T_TOPK;
    case Kind::DynamicSampling: return T_DYNAMIC_SAMPLING;
    case Kind::TableDynamicSampling: return T_TABLE_DYNAMIC_SAMPLING;
    case Kind::Snapshot: return T_TABLE_SNAPSHOT_QUERY_SCN;
    case Kind::TableOptions: return T_TABLE_OPTION_LIST;
    case Kind::TableCharset: return T_CHARSET;
    case Kind::Collation: return T_COLLATION;
    case Kind::SetCollation: return T_FUN_SYS_SET_COLLATION;
    case Kind::Compression: return T_COMPRESSION;
    case Kind::StoreFormat: return T_STORE_FORMAT;
    case Kind::TableAutoIncrement: return T_AUTO_INCREMENT;
    case Kind::BlockSize: return T_BLOCK_SIZE;
    case Kind::TabletSize: return T_TABLET_SIZE;
    case Kind::PctFree: return T_PCTFREE;
    case Kind::Index: return T_INDEX;
    case Kind::IndexColumns: return T_INDEX_COLUMN_LIST;
    case Kind::IndexColumn: return T_SORT_COLUMN_KEY;
    case Kind::ParserName: return T_PARSER_NAME;
    case Kind::ParserProperties: return T_PARSER_PROPERTIES;
    case Kind::ParserMinTokenSize: return T_PARSER_MIN_TOKEN_SIZE;
    case Kind::ParserMaxTokenSize: return T_PARSER_MAX_TOKEN_SIZE;
    case Kind::ParserNgramTokenSize: return T_PARSER_NGRAM_TOKEN_SIZE;
    case Kind::ParserStopwordTable: return T_PARSER_STOPWORD_TABLE;
    case Kind::ParserDictTable: return T_PARSER_DICT_TABLE;
    case Kind::ParserQuantifierTable: return T_PARSER_QUANTIFIER_TABLE;
    case Kind::ParserIkMode: return T_IK_MODE;
    case Kind::ParserMinNgramSize: return T_PARSER_MIN_NGRAM_SIZE;
    case Kind::ParserMaxNgramSize: return T_PARSER_MAX_NGRAM_SIZE;
    case Kind::MatchColumns: return T_MATCH_COLUMN_LIST;
    case Kind::MatchAgainst: return T_FUN_MATCH_AGAINST;
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
    case Kind::Sample: return T_SAMPLE_SCAN;
    case Kind::SampleInt: return T_SFU_INT;
    case Kind::SampleDecimal: return T_SFU_DECIMAL;
    case Kind::SampleBase: return T_BASE;
    case Kind::SampleIncremental: return T_INCR;
    case Kind::Ident: return T_IDENT;
    case Kind::Column: return T_COLUMN_REF;
    case Kind::Star: return T_STAR;
    case Kind::Int: return T_INT;
    case Kind::Uint: return T_UINT64;
    case Kind::Number: return T_NUMBER;
    case Kind::Double: return T_DOUBLE;
    case Kind::String: return T_VARCHAR;
    case Kind::StringColumnName: return T_CONCAT_STRING;
    case Kind::CollectionType: return T_COLLECTION;
    case Kind::ArrayLiteral: return T_FUN_SYS_ARRAY;
    case Kind::MapLiteral: return T_FUN_SYS_MAP;
    case Kind::ArrayAgg: return T_FUNC_SYS_ARRAY_AGG;
    case Kind::ArrayMap: return T_FUNC_SYS_ARRAY_MAP;
    case Kind::ArrayFilter: return T_FUNC_SYS_ARRAY_FILTER;
    case Kind::ArrayFirst: return T_FUNC_SYS_ARRAY_FIRST;
    case Kind::ArraySortBy: return T_FUNC_SYS_ARRAY_SORTBY;
    case Kind::Lambda: return T_FUNC_SYS_LAMBDA;
    case Kind::VectorDistance: return T_FUN_SYS_VECTOR_DISTANCE;
    case Kind::VectorSimilarity: return T_FUN_SYS_VECTOR_SIMILARITY;
    case Kind::Approximate: return T_APPROX;
    case Kind::VectorParameters: return T_VEC_INDEX_PARAMS;
    case Kind::CharsetLiteral: return T_VARCHAR;
    case Kind::CharsetName: return T_CHAR_CHARSET;
    case Kind::ConvertTableCharset: return T_CONVERT_TO_CHARACTER;
    case Kind::TableCollection: return T_TABLE_COLLECTION_EXPRESSION;
    case Kind::DateLiteral: return T_DATE;
    case Kind::TimeLiteral: return T_TIME;
    case Kind::TimestampLiteral: return T_TIMESTAMP;
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
    case Kind::Function:
    case Kind::StringConcat: return T_FUN_SYS;
    case Kind::Cast: return T_FUN_SYS;
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
    case Kind::Stddev: return T_FUN_STDDEV;
    case Kind::StddevPop: return T_FUN_STDDEV_POP;
    case Kind::StddevSamp: return T_FUN_STDDEV_SAMP;
    case Kind::Variance: return T_FUN_VARIANCE;
    case Kind::VarPop: return T_FUN_VAR_POP;
    case Kind::VarSamp: return T_FUN_VAR_SAMP;
    case Kind::BitAnd: return T_FUN_SYS_BIT_AND;
    case Kind::BitOr: return T_FUN_SYS_BIT_OR;
    case Kind::BitXor: return T_FUN_SYS_BIT_XOR;
    case Kind::All: return T_ALL;
    case Kind::Any: return T_ANY;
    case Kind::Exists: return T_OP_EXISTS;
    case Kind::JoinedTable: return T_JOINED_TABLE;
    case Kind::InnerJoin: return T_JOIN_INNER;
    case Kind::LeftJoin: return T_JOIN_LEFT;
    case Kind::RightJoin: return T_JOIN_RIGHT;
    case Kind::FullJoin: return T_JOIN_FULL;
    case Kind::UsePartition: return T_USE_PARTITION;
    case Kind::Names: return T_NAME_LIST;
    case Kind::TableIndexHints: return T_INDEX_HINT_LIST;
    case Kind::TableIndexHint: return T_INDEX_HINT_DEF;
    case Kind::Use: return T_USE;
    case Kind::Force: return T_FORCE;
    case Kind::ListPartition: return T_LIST_PARTITION;
    case Kind::RangePartition: return T_RANGE_PARTITION;
    case Kind::ListColumnsPartition: return T_LIST_COLUMNS_PARTITION;
    case Kind::RangeColumnsPartition: return T_RANGE_COLUMNS_PARTITION;
    case Kind::Partitions: return T_PARTITION_LIST;
    case Kind::HashSubpartitions: return T_HASH_SUBPARTITION_LIST;
    case Kind::RangeSubpartitions: return T_RANGE_SUBPARTITION_LIST;
    case Kind::ListSubpartitions: return T_LIST_SUBPARTITION_LIST;
    case Kind::HashPartitionElement: return T_PARTITION_HASH_ELEMENT;
    case Kind::ListPartitionElement: return T_PARTITION_LIST_ELEMENT;
    case Kind::RangePartitionElement: return T_PARTITION_RANGE_ELEMENT;
    case Kind::MaxValue: return T_MAXVALUE;
    case Kind::CheckConstraint: return T_CHECK_CONSTRAINT;
    case Kind::Enforced: return T_ENFORCED_CONSTRAINT;
    case Kind::NotEnforced: return T_NOENFORCED_CONSTRAINT;
    case Kind::UserAssignment: return T_OP_ASSIGN;
    case Kind::UserVariableTarget: return T_LEFT_VALUE;
    case Kind::ColumnList: return T_COLUMN_LIST;
    case Kind::GroupBy: return T_GROUPBY_CLAUSE;
    case Kind::WithRollup: return T_WITH_ROLLUP_CLAUSE;
    case Kind::Rollup: return T_ROLLUP;
    case Kind::DefaultInt: return T_DEFAULT_INT;
    case Kind::EnumType: return T_ENUM;
    case Kind::SetType: return T_SET;
    case Kind::StringList: return T_STRING_LIST;
    case Kind::BinaryAttribute: return T_BINARY;
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
    case Kind::SetTransaction: return T_TRANSACTION;
    case Kind::TransactionCharacteristics: return T_TRANSACTION_CHARACTERISTICS;
    case Kind::Commit: return T_COMMIT;
    case Kind::Rollback: return T_ROLLBACK;
    case Kind::UseDatabase: return T_USE_DATABASE;
    case Kind::CreateDatabase: return T_CREATE_DATABASE;
    case Kind::DatabaseOptions: return T_DATABASE_OPTION_LIST;
    case Kind::DropDatabase: return T_DROP_DATABASE;
    case Kind::IfExists: return T_IF_EXISTS;
    case Kind::IfNotExists: return T_IF_NOT_EXISTS;
    case Kind::CreateTable: return T_CREATE_TABLE;
    case Kind::CreateView: return T_CREATE_VIEW;
    case Kind::GroupConcat: return T_FUN_GROUP_CONCAT;
    case Kind::Separator: return T_SEPARATOR_CLAUSE;
    case Kind::GatherStatistics: return T_GATHER_OPTIMIZER_STATISTICS;
    case Kind::NoGatherStatistics: return T_NO_GATHER_OPTIMIZER_STATISTICS;
    case Kind::DropView: return T_DROP_VIEW;
    case Kind::ViewList: return T_VIEW_LIST;
    case Kind::ShowCreateView: return T_SHOW_CREATE_VIEW;
    case Kind::CreateTableLike: return T_CREATE_TABLE_LIKE;
    case Kind::Charset: return T_CHARSET;
    case Kind::DropTable: return T_DROP_TABLE;
    case Kind::TruncateTable: return T_TRUNCATE_TABLE;
    case Kind::PurgeRecyclebin: return T_PURGE_RECYCLEBIN;
    case Kind::PurgeTable: return T_PURGE_TABLE;
    case Kind::PurgeIndex: return T_PURGE_INDEX;
    case Kind::PurgeDatabase: return T_PURGE_DATABASE;
    case Kind::RenameTables: return T_RENAME_TABLE;
    case Kind::RenameTableAction: return T_RENAME_TABLE_ACTION;
    case Kind::AlterTable: return T_ALTER_TABLE;
    case Kind::AlterTableActions: return T_ALTER_TABLE_ACTION_LIST;
    case Kind::AlterTableOption: return T_ALTER_TABLE_OPTION;
    case Kind::TableRename: return T_TABLE_RENAME;
    case Kind::AlterColumnOption: return T_ALTER_COLUMN_OPTION;
    case Kind::ColumnAdd: return T_COLUMN_ADD;
    case Kind::ColumnDrop: return T_COLUMN_DROP;
    case Kind::ColumnModify: return T_COLUMN_MODIFY;
    case Kind::ColumnChange: return T_COLUMN_CHANGE;
    case Kind::ColumnRename: return T_COLUMN_RENAME;
    case Kind::ColumnAlter: return T_COLUMN_ALTER;
    case Kind::ColumnFirst: return T_COLUMN_ADD_FIRST;
    case Kind::ColumnBefore: return T_COLUMN_ADD_BEFORE;
    case Kind::ColumnAfter: return T_COLUMN_ADD_AFTER;
    case Kind::AlterIndexOption: return T_ALTER_INDEX_OPTION;
    case Kind::IndexAdd: return T_INDEX_ADD;
    case Kind::IndexDrop: return T_INDEX_DROP;
    case Kind::IndexRename: return T_INDEX_RENAME;
    case Kind::PrimaryKeyDrop: return T_PRIMARY_KEY_DROP;
    case Kind::ShowIndexes: return T_SHOW_INDEXES;
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
    case Kind::OnUpdate: return T_ON_UPDATE;
    case Kind::ForeignKey: return T_FOREIGN_KEY;
    case Kind::ForeignKeyDrop: return T_FOREIGN_KEY_DROP;
    case Kind::AlterForeignKey: return T_ALTER_FOREIGN_KEY_OPTION;
    case Kind::DropConstraint: return T_DROP_CONSTRAINT;
    case Kind::ReferenceOptions: return T_REFERENCE_OPTION_LIST;
    case Kind::ReferenceOption: return T_REFERENCE_OPTION;
    case Kind::Restrict: return T_RESTRICT;
    case Kind::Cascade: return T_CASCADE;
    case Kind::SetNull: return T_SET_NULL;
    case Kind::NoAction: return T_NO_ACTION;
    case Kind::SetDefault: return T_SET_DEFAULT;
    case Kind::ColumnComment: return T_COMMENT;
    case Kind::GeneratedColumn: return T_GENERATED_COLUMN;
    case Kind::Always: return T_CONSTR_ALWAYS;
    case Kind::VirtualColumn: return T_VIRTUAL_COLUMN;
    case Kind::StoredColumn: return T_STORED_COLUMN;
    case Kind::SystemVariable: return T_SYSTEM_VARIABLE;
    case Kind::UserVariable: return T_USER_VARIABLE_IDENTIFIER;
    case Kind::GetUserVariable: return T_OP_GET_USER_VAR;
    case Kind::VariableSet: return T_VARIABLE_SET;
    case Kind::VariableValue: return T_VAR_VAL;
    case Kind::SetNames: return T_SET_NAMES;
    case Kind::SetCharset: return T_SET_CHARSET;
    case Kind::ShowDatabases: return T_SHOW_DATABASES;
    case Kind::ShowCharset: return T_SHOW_CHARSET;
    case Kind::ShowCollation: return T_SHOW_COLLATION;
    case Kind::ShowTables: return T_SHOW_TABLES;
    case Kind::ShowColumns: return T_SHOW_COLUMNS;
    case Kind::ShowVariables: return T_SHOW_VARIABLES;
    case Kind::ShowParameters: return T_SHOW_PARAMETERS;
    case Kind::ShowProcesslist: return T_SHOW_PROCESSLIST;
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
    case Kind::Freeze: return T_FREEZE;
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
    case Kind::BeginOutline: return T_BEGIN_OUTLINE_DATA;
    case Kind::EndOutline: return T_END_OUTLINE_DATA;
    case Kind::HintQbNames: return T_QB_NAME_LIST;
    case Kind::Materialize: return T_MATERIALIZE;
    case Kind::CoalesceSubqueries: return T_COALESCE_SQ;
    case Kind::NoCoalesceSubqueries: return T_NO_COALESCE_SQ;
    case Kind::HashSet: return T_USE_HASH_SET;
    case Kind::NoHashSet: return T_NO_USE_HASH_SET;
    case Kind::PqDistribute: return T_PQ_DISTRIBUTE;
    case Kind::PqMap: return T_PQ_MAP;
    case Kind::PqSet: return T_PQ_SET;
    case Kind::DistributeMethods: return T_DISTRIBUTE_METHOD_LIST;
    case Kind::UseConcat: return T_USE_CONCAT;
    case Kind::NoExpand: return T_NO_EXPAND;
    case Kind::PqGroupBy: return T_PQ_GBY_HINT;
    case Kind::PqDistinct: return T_PQ_DISTINCT_HINT;
    case Kind::GroupByPushdown: return T_GBY_PUSHDOWN;
    case Kind::NoGroupByPushdown: return T_NO_GBY_PUSHDOWN;
    case Kind::HashDistinct: return T_USE_HASH_DISTINCT;
    case Kind::NoHashDistinct: return T_NO_USE_HASH_DISTINCT;
    case Kind::DistinctPushdown: return T_DISTINCT_PUSHDOWN;
    case Kind::NoDistinctPushdown: return T_NO_DISTINCT_PUSHDOWN;
    case Kind::DistributeAll: return T_DISTRIBUTE_ALL;
    case Kind::DistributeNone: return T_DISTRIBUTE_NONE;
    case Kind::DistributePartition: return T_DISTRIBUTE_PARTITION;
    case Kind::DistributeRandom: return T_DISTRIBUTE_RANDOM;
    case Kind::DistributeHash: return T_DISTRIBUTE_HASH;
    case Kind::DistributeBroadcast: return T_DISTRIBUTE_BROADCAST;
    case Kind::DistributeLocal: return T_DISTRIBUTE_LOCAL;
    case Kind::DistributeBc2Host: return T_DISTRIBUTE_BC2HOST;
    case Kind::DistributeRange: return T_DISTRIBUTE_RANGE;
    case Kind::DistributeList: return T_DISTRIBUTE_LIST;
    case Kind::DistributeBasic: return T_DISTRIBUTE_BASIC;
    case Kind::DistributeHashLocal: return T_DISTRIBUTE_HASH_LOCAL;
    case Kind::HintRelation: return T_RELATION_FACTOR_IN_HINT;
    case Kind::HintTables: return T_RELATION_FACTOR_IN_HINT_LIST;
    case Kind::JoinHintTables: return T_RELATION_FACTOR_IN_USE_JOIN_HINT_LIST;
    case Kind::Leading: return T_LEADING;
    case Kind::UseNl: return T_USE_NL;
    case Kind::UseNlMaterialization: return T_USE_NL_MATERIALIZATION;
    case Kind::NoUseNl: return T_NO_USE_NL;
    case Kind::NoUseNlMaterialization: return T_NO_USE_NL_MATERIALIZATION;
    case Kind::UseHash: return T_USE_HASH;
    case Kind::NoUseHash: return T_NO_USE_HASH;
    case Kind::UseMerge: return T_USE_MERGE;
    case Kind::NoUseMerge: return T_NO_USE_MERGE;
    case Kind::QbName: return T_QB_NAME;
    case Kind::OptParam: return T_OPT_PARAM_HINT;
    case Kind::NoRewrite: return T_NO_REWRITE;
    case Kind::MergeView: return T_MERGE_HINT;
    case Kind::NoMergeView: return T_NO_MERGE_HINT;
    case Kind::OuterToInner: return T_OUTER_TO_INNER;
    case Kind::NoOuterToInner: return T_NO_OUTER_TO_INNER;
    case Kind::Unnest: return T_UNNEST;
    case Kind::NoUnnest: return T_NO_UNNEST;
    case Kind::SimplifyGroupBy: return T_SIMPLIFY_GROUP_BY;
    case Kind::NoSimplifyGroupBy: return T_NO_SIMPLIFY_GROUP_BY;
    case Kind::PredicateDeduce: return T_PRED_DEDUCE;
    case Kind::NoPredicateDeduce: return T_NO_PRED_DEDUCE;
    case Kind::ProjectPrune: return T_PROJECT_PRUNE;
    case Kind::NoProjectPrune: return T_NO_PROJECT_PRUNE;
    case Kind::SemiToInner: return T_SEMI_TO_INNER;
    case Kind::NoSemiToInner: return T_NO_SEMI_TO_INNER;
    case Kind::EliminateJoin: return T_ELIMINATE_JOIN;
    case Kind::NoEliminateJoin: return T_NO_ELIMINATE_JOIN;
    case Kind::LeftToAnti: return T_LEFT_TO_ANTI;
    case Kind::NoLeftToAnti: return T_NO_LEFT_TO_ANTI;
    case Kind::PlaceGroupBy: return T_PLACE_GROUP_BY;
    case Kind::NoPlaceGroupBy: return T_NO_PLACE_GROUP_BY;
    case Kind::WindowMagic: return T_WIN_MAGIC;
    case Kind::NoWindowMagic: return T_NO_WIN_MAGIC;
    case Kind::PushPredCte: return T_PUSH_PRED_CTE;
    case Kind::NoPushPredCte: return T_NO_PUSH_PRED_CTE;
    case Kind::ReplaceConst: return T_REPLACE_CONST;
    case Kind::NoReplaceConst: return T_NO_REPLACE_CONST;
    case Kind::SimplifyOrderBy: return T_SIMPLIFY_ORDER_BY;
    case Kind::NoSimplifyOrderBy: return T_NO_SIMPLIFY_ORDER_BY;
    case Kind::SimplifyDistinct: return T_SIMPLIFY_DISTINCT;
    case Kind::NoSimplifyDistinct: return T_NO_SIMPLIFY_DISTINCT;
    case Kind::SimplifyWinfunc: return T_SIMPLIFY_WINFUNC;
    case Kind::NoSimplifyWinfunc: return T_NO_SIMPLIFY_WINFUNC;
    case Kind::SimplifyExpr: return T_SIMPLIFY_EXPR;
    case Kind::NoSimplifyExpr: return T_NO_SIMPLIFY_EXPR;
    case Kind::SimplifyLimit: return T_SIMPLIFY_LIMIT;
    case Kind::NoSimplifyLimit: return T_NO_SIMPLIFY_LIMIT;
    case Kind::SimplifySubquery: return T_SIMPLIFY_SUBQUERY;
    case Kind::NoSimplifySubquery: return T_NO_SIMPLIFY_SUBQUERY;
    case Kind::FastMinmax: return T_FAST_MINMAX;
    case Kind::NoFastMinmax: return T_NO_FAST_MINMAX;
    case Kind::SimplifySet: return T_SIMPLIFY_SET;
    case Kind::NoSimplifySet: return T_NO_SIMPLIFY_SET;
    case Kind::PushLimit: return T_PUSH_LIMIT;
    case Kind::NoPushLimit: return T_NO_PUSH_LIMIT;
    case Kind::PullupExpr: return T_PULLUP_EXPR;
    case Kind::NoPullupExpr: return T_NO_PULLUP_EXPR;
    case Kind::AggrFirstUnnest: return T_AGGR_FIRST_UNNEST;
    case Kind::NoAggrFirstUnnest: return T_NO_AGGR_FIRST_UNNEST;
    case Kind::JoinFirstUnnest: return T_JOIN_FIRST_UNNEST;
    case Kind::NoJoinFirstUnnest: return T_NO_JOIN_FIRST_UNNEST;
    case Kind::Decorrelate: return T_DECORRELATE;
    case Kind::NoDecorrelate: return T_NO_DECORRELATE;
    case Kind::UseLateMaterialization: return T_USE_LATE_MATERIALIZATION;
    case Kind::NoUseLateMaterialization: return T_NO_USE_LATE_MATERIALIZATION;
    case Kind::TransformDistinctAgg: return T_TRANSFORM_DISTINCT_AGG;
    case Kind::NoTransformDistinctAgg: return T_NO_TRANSFORM_DISTINCT_AGG;
    case Kind::PushSubq: return T_PUSH_SUBQ;
    case Kind::NoPushSubq: return T_NO_PUSH_SUBQ;
    case Kind::Inline: return T_INLINE;
    case Kind::UseDas: return T_USE_DAS_HINT;
    case Kind::NoUseDas: return T_NO_USE_DAS_HINT;
    case Kind::UsePlanCache: return T_USE_PLAN_CACHE;
    case Kind::WindowFunction: return T_WINDOW_FUNCTION;
    case Kind::Window: return T_WIN_NEW_GENERALIZED_WINDOW;
    case Kind::WindowSpec: return T_WIN_GENERALIZED_WINDOW;
    case Kind::WindowFrame: return T_WIN_WINDOW;
    case Kind::WindowBound: return T_WIN_BOUND;
    case Kind::WindowInterval: return T_WIN_INTERVAL;
    case Kind::HiddenInt: return T_INT;
    case Kind::DateUnit: return T_INT;
    case Kind::RowNumber: return T_WIN_FUN_ROW_NUMBER;
    case Kind::Rank: return T_WIN_FUN_RANK;
    case Kind::DenseRank: return T_WIN_FUN_DENSE_RANK;
    case Kind::PercentRank: return T_WIN_FUN_PERCENT_RANK;
    case Kind::CumeDist: return T_WIN_FUN_CUME_DIST;
    case Kind::Ntile: return T_WIN_FUN_NTILE;
    case Kind::FirstValue: return T_WIN_FUN_FIRST_VALUE;
    case Kind::LastValue: return T_WIN_FUN_LAST_VALUE;
    case Kind::NthValue: return T_WIN_FUN_NTH_VALUE;
    case Kind::Lead: return T_WIN_FUN_LEAD;
    case Kind::Lag: return T_WIN_FUN_LAG;
    case Kind::First: return T_FIRST;
    case Kind::Last: return T_LAST;
    case Kind::Respect: return T_RESPECT;
    case Kind::ReadConsistency: return T_READ_CONSISTENCY;
    case Kind::NamedWindows: return T_WIN_NAMED_WINDOWS;
    case Kind::NamedWindow: return T_WIN_NAMED_WINDOW;
    case Kind::CreateUser: return T_CREATE_USER;
    case Kind::DropUser: return T_DROP_USER;
    case Kind::CreateRole: return T_CREATE_ROLE;
    case Kind::DropRole: return T_DROP_ROLE;
    case Kind::GrantRole: return T_GRANT_ROLE;
    case Kind::RevokeRole: return T_REVOKE_ROLE;
    case Kind::SystemRevoke: return T_SYSTEM_REVOKE;
    case Kind::WithAdminOption: return T_WITH_ADMIN_OPTION;
    case Kind::IgnoreUnknownUser: return T_IGNORE_UNKNOWN_USER;
    case Kind::SetRole: return T_SET_ROLE;
    case Kind::DefaultRole: return T_DEFAULT_ROLE;
    case Kind::AlterUserDefaultRole: return T_ALTER_USER_DEFAULT_ROLE;
    case Kind::Kill: return T_KILL;
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
    case Kind::PlNamedArgument: return T_SP_CPARAM;
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
  else if (name == "tinytext" || name == "tinyblob") type = T_TINYTEXT;
  else if (name == "mediumtext" || name == "mediumblob" || name == "string") type = T_MEDIUMTEXT;
  else if (name == "longtext" || name == "longblob") type = T_LONGTEXT;
  else if (name == "datetime") type = T_DATETIME;
  else if (name == "timestamp") type = T_TIMESTAMP;
  else if (name == "time") type = T_TIME;
  else if (name == "date") type = T_DATE;
  else if (name == "json") type = T_JSON;
  else if (name == "year") type = T_YEAR;
  else if (name == "bit") type = T_BIT;
  else if (name == "geometry") type = T_GEOMETRY;
  if (type == T_INVALID) return common::OB_ERR_UNEXPECTED;
  const bool integer = type == T_TINYINT || type == T_SMALLINT || type == T_MEDIUMINT
      || type == T_INT32 || type == T_INT;
  const bool string = type == T_CHAR || type == T_VARCHAR || type == T_TINYTEXT || type == T_TEXT || type == T_MEDIUMTEXT || type == T_LONGTEXT;
  const bool binary = name == "binary" || name == "varbinary" || name == "tinyblob" || name == "blob" || name == "mediumblob" || name == "longblob";
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
  size_t size = args.size();
  while (size > 0 && (args[size - 1].kind == Kind::Charset || args[size - 1].kind == Kind::Collation)) --size;
  const int32_t precision = (size == 0) ? 0 : std::min<uint64_t>(args[0].value, INT32_MAX);
  const int16_t short_precision = std::min<int32_t>(precision, INT16_MAX);
  const int16_t scale = size < 2 ? 0 : std::min<uint64_t>(args[1].value, INT16_MAX);
  if (cast) {
    node->int16_values_[OB_NODE_CAST_TYPE_IDX] = type;
    node->param_num_ = size;
    if (type == T_CHAR || type == T_VARCHAR) {
      node->int16_values_[OB_NODE_CAST_COLL_IDX] = binary ? 63 : 0;
      node->int32_values_[OB_NODE_CAST_C_LEN_IDX] = size == 0 ? -1 : precision;
      if (size < args.size()) {
        const auto &charset = args[size];
        node->str_value_ = parse_strndup(charset.text.data(), charset.text.size(), pool);
        if (node->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
        node->str_len_ = charset.text.size();
        node->param_num_ += charset.value;
      }
    } else if (type == T_FLOAT || type == T_DOUBLE || type == T_NUMBER) {
      node->int16_values_[OB_NODE_CAST_N_PREC_IDX] = size == 0
          ? (type == T_NUMBER ? 10 : type == T_FLOAT ? 0 : -1) : short_precision;
      node->int16_values_[OB_NODE_CAST_N_SCALE_IDX] = type == T_NUMBER ? scale : -1;
    } else if (type == T_DATETIME || type == T_DATE || type == T_TIME || type == T_YEAR) {
      node->int16_values_[OB_NODE_CAST_N_SCALE_IDX] = short_precision;
    }
  } else if (string) {
    node->int32_values_[0] = size == 0 && type == T_CHAR ? 1 : precision;
    node->int32_values_[1] = name == "string" ? 2 : binary;
  } else if (type == T_BIT) {
    node->int16_values_[0] = size == 0 ? 1 : short_precision;
  } else if (integer) {
    node->int16_values_[0] = size == 0 ? -1 : short_precision;
    if (name == "bool" || name == "boolean") {
      node->int16_values_[0] = 1;
      node->int16_values_[1] = name == "bool" ? 1 : 2;
    }
  } else if (type == T_DATETIME || type == T_TIMESTAMP || type == T_TIME) {
    node->int16_values_[1] = short_precision;
  } else if (name == "decimal" || name == "float" || name == "double") {
    node->int16_values_[0] = size == 0 ? (name == "decimal" ? 10 : -1) : short_precision;
    node->int16_values_[1] = name != "decimal" && size < 2 ? -1 : scale;
  }
  if (!cast && !string) {
    node->int16_values_[2] = (source.value & 2) != 0;
    node->int16_values_[3] = (source.value & 1) != 0;
  }
  for (size_t i = size; i < args.size() && node->num_child_ >= 2; ++i) {
    const auto &attribute = args[i];
    ParseNode *child = new_node(pool, attribute.kind == Kind::Charset ? T_CHARSET : T_COLLATION, 0);
    if (child == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    child->str_value_ = parse_strndup(attribute.text.data(), attribute.text.size(), pool);
    if (child->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    child->str_len_ = attribute.text.size();
    node->children_[attribute.kind == Kind::Charset ? 0 : 1] = child;
  }
  node->sql_str_off_ = source.start;
  return common::OB_SUCCESS;
}

int build_node(const common::ObString &sql, const common::ObString &expression_sql, const peg::Node &source, size_t parameters,
               void *pool, ParseNode *&node, bool pl = false)
{
  node = nullptr;
  if (source.kind == Kind::Missing) return common::OB_SUCCESS;
  if (source.kind == Kind::DataType || source.kind == Kind::CastType) return build_type(source, pool, node);
  if (source.kind == Kind::Parenthesized) {
    const int ret = build_node(sql, expression_sql, source.children[0], parameters, pool, node, pl);
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
                             PARSE_SELECT_FOR_UPD, PARSE_SELECT_HINTS, PARSE_SELECT_INTO_EXTRA,
                             PARSE_SELECT_APPROX, PARSE_SELECT_VECTOR_INDEX_PARAMS, PARSE_SELECT_WITH};
  for (size_t child = 0; child < source.children.size(); ++child) {
    const size_t slot = type == T_SELECT ? select_slots[child] : child;
    const int ret = build_node(sql, expression_sql, source.children[child], parameters, pool, node->children_[slot], pl);
    if (ret != common::OB_SUCCESS) return ret;
  }
  if (type == T_COLUMN_DEFINITION && node->children_[1]->num_child_ >= 2 && node->children_[2] != nullptr) {
    ParseNode *attributes = node->children_[2];
    for (int i = 0; i < attributes->num_child_; ++i) {
      if (attributes->children_[i]->type_ == T_COLLATION) node->children_[1]->children_[1] = attributes->children_[i];
    }
  }
  if (type == T_CREATE_VIEW) node->reserved_ = source.value;
  if (source.kind == Kind::Asc && source.value == 2 && source.start == source.end) node->is_empty_ = true;
  if (source.kind == Kind::StringConcat) node->children_[0]->reserved_ = 1;
  if ((type == T_COLUMN_DEFINITION && node->num_child_ == 7) || (type == T_CHECK_CONSTRAINT && node->num_child_ == 3)
      || (type == T_SORT_COLUMN_KEY && source.children[0].kind != Kind::Ident)) {
    const size_t slot = type == T_COLUMN_DEFINITION ? 3 : type == T_CHECK_CONSTRAINT ? 1 : 0;
    const auto &expression = source.children[slot];
    ParseNode *child = node->children_[slot];
    child->str_len_ = expression.end - expression.start;
    child->str_value_ = parse_strndup(expression_sql.ptr() + expression.start, child->str_len_, pool);
    if (child->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  }
  if (type != T_FUN_SYS || source.kind == Kind::Cast) node->value_ = type == T_SELECT ? parameters : source.value;
  if (type == T_REFERENCE_OPTION) {
    node->int32_values_[0] = source.value ? T_DELETE : T_UPDATE;
    node->int32_values_[1] = node->children_[0]->type_;
    node->num_child_ = 0;
  }
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
  const bool temporal = type == T_DATE || type == T_TIME || type == T_TIMESTAMP;
  if (temporal) node->is_copy_raw_text_ = true;
  const bool literal = temporal || type == T_INT || type == T_UINT64 || type == T_NUMBER || type == T_DOUBLE
      || type == T_VARCHAR || type == T_HEX_STRING || type == T_BOOL || type == T_NULL || type == T_QUESTIONMARK;
  if (literal && source.kind != Kind::HiddenInt && source.kind != Kind::HiddenNull && source.kind != Kind::CacheType && source.kind != Kind::DateUnit) {
    node->raw_text_ = parse_strndup(raw, raw_length, pool);
    if (node->raw_text_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->text_len_ = raw_length;
    node->param_num_ = 1;
  }
  const char *text = source.text.data();
  size_t length = source.text.size();
  if (type == T_SELECT || type == T_PROJECT_STRING || type == T_COLUMN_REF || type == T_OP_POS || type == T_OP_NEG
      || source.kind == Kind::Binary || source.kind == Kind::Between || source.kind == Kind::In
      || source.kind == Kind::Is || source.kind == Kind::Window || source.kind == Kind::PlSql
      || source.kind == Kind::RoutineParameters || source.kind == Kind::PlDefault
      || source.kind == Kind::SubProcedureDeclaration || source.kind == Kind::SubFunctionDeclaration
      || source.kind == Kind::ObjectAccess || source.kind == Kind::KeyPartition || source.kind == Kind::HashPartition
      || source.kind == Kind::ListPartition || source.kind == Kind::ListColumnsPartition
      || source.kind == Kind::RangePartition || source.kind == Kind::RangeColumnsPartition
      || source.kind == Kind::VectorParameters
      || (source.kind == Kind::DateUnit && raw_length != 0)) {
    text = raw;
    length = raw_length;
  }
  if (source.kind == Kind::CharsetLiteral) {
    ParseNode *literal = node->children_[1];
    node->str_value_ = literal->str_value_;
    node->str_len_ = literal->str_len_;
    if (literal->type_ == T_HEX_STRING) node->num_child_ = 1;
  } else if (type == T_HEX_STRING) {
    const bool bits = source.kind == Kind::BitString;
    const size_t size = bits ? ob_parse_bit_string_len(length) : ob_parse_binary_len(length);
    char *bytes = static_cast<char *>(parse_malloc(size + 1, pool));
    if (bytes == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    if (bits) ob_parse_bit_string(text, length, bytes);
    else ob_parse_binary(text, length, bytes);
    bytes[size] = '\0';
    node->str_value_ = bytes;
    node->str_len_ = size;
  } else if (length != 0 || temporal || type == T_VARCHAR || type == T_IDENT) {
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
  if (source.kind == Kind::HiddenInt || source.kind == Kind::HiddenNull || source.kind == Kind::CharsetName) node->is_hidden_const_ = true;
  if (source.kind == Kind::DateUnit) {
    node->is_hidden_const_ = true;
    node->is_date_unit_ = source.start != source.end;
    for (int unit = DATE_UNIT_MICROSECOND; unit < DATE_UNIT_MAX; ++unit) {
      if (source.text == ob_date_unit_type_str(static_cast<ObDateUnitType>(unit))) {
        node->value_ = unit;
        break;
      }
    }
  }
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
    const ParseNode *literal = node->children_[0];
    if (literal->num_child_ == 2 && literal->children_[1]->type_ == T_VARCHAR) literal = literal->children_[1];
    if (literal->num_child_ == 1 && literal->children_[0]->type_ == T_CONCAT_STRING) literal = literal->children_[0];
    node->str_value_ = literal->str_value_;
    node->str_len_ = literal->str_len_;
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
  const common::ObString expression_sql = tree.expanded_sql.empty()
      ? sql : common::ObString(tree.expanded_sql.size(), tree.expanded_sql.data());
  const int ret = build_node(sql, expression_sql, tree.root, tree.parameters, result.malloc_pool_, query, routine_body);
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
