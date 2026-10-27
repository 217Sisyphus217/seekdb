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
#include "sql/parser/parse_node.h"
#include "sql/parser/parse_malloc.h"
#include "lib/string/ob_string.h"
#include "share/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include <cctype>

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
    case Kind::Select: return T_SELECT;
    case Kind::ProjectList: return T_PROJECT_LIST;
    case Kind::Projection: return T_PROJECT_STRING;
    case Kind::Alias: return T_ALIAS;
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
    default: return T_INVALID;
  }
}

int build_node(const common::ObString &sql, const peg::Node &source, size_t parameters,
               void *pool, ParseNode *&node)
{
  node = nullptr;
  if (source.kind == Kind::Missing) return common::OB_SUCCESS;
  if (source.kind == Kind::Parenthesized) {
    const int ret = build_node(sql, source.children[0], parameters, pool, node);
    if (ret != common::OB_SUCCESS) return ret;
    node->is_assigned_from_child_ = true;
    return common::OB_SUCCESS;
  }
  const ObItemType type = node_type(source);
  if (type == T_INVALID) return common::OB_ERR_UNEXPECTED;
  node = new_node(pool, type, type == T_SELECT ? PARSE_SELECT_MAX_IDX : source.children.size());
  if (node == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  const int select_slots[] = {PARSE_SELECT_DISTINCT, PARSE_SELECT_SELECT, PARSE_SELECT_FROM,
                             PARSE_SELECT_WHERE, PARSE_SELECT_DYNAMIC_GROUP, PARSE_SELECT_DYNAMIC_HAVING,
                             PARSE_SELECT_ORDER, PARSE_SELECT_LIMIT};
  for (size_t child = 0; child < source.children.size(); ++child) {
    const size_t slot = type == T_SELECT ? select_slots[child] : child;
    const int ret = build_node(sql, source.children[child], parameters, pool, node->children_[slot]);
    if (ret != common::OB_SUCCESS) return ret;
  }
  if (type == T_SELECT && node->children_[PARSE_SELECT_DISTINCT] != nullptr) {
    ParseNode *options = new_node(pool, T_QEURY_EXPRESSION_LIST, 1);
    if (options == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    options->children_[0] = node->children_[PARSE_SELECT_DISTINCT];
    node->children_[PARSE_SELECT_DISTINCT] = options;
  }
  node->value_ = type == T_SELECT ? parameters : source.value;
  node->sql_str_off_ = source.start;
  node->raw_sql_offset_ = source.start;
  node->stmt_loc_.first_column_ = source.start + 1;
  node->stmt_loc_.last_column_ = source.end;
  const char *raw = sql.ptr() + source.start;
  const size_t raw_length = source.end - source.start;
  const bool literal = type == T_INT || type == T_UINT64 || type == T_NUMBER || type == T_DOUBLE
      || type == T_VARCHAR || type == T_BOOL || type == T_NULL || type == T_QUESTIONMARK;
  if (literal) {
    node->raw_text_ = parse_strndup(raw, raw_length, pool);
    if (node->raw_text_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->text_len_ = raw_length;
    node->param_num_ = 1;
  }
  const char *text = source.text.data();
  size_t length = source.text.size();
  if (type == T_PROJECT_STRING || type == T_COLUMN_REF || type == T_OP_POS || type == T_OP_NEG
      || source.kind == Kind::Binary || source.kind == Kind::Between || source.kind == Kind::In
      || source.kind == Kind::Is) {
    text = raw;
    length = raw_length;
  }
  if (length != 0 || type == T_VARCHAR) {
    node->str_value_ = parse_strndup(text, length, pool);
    if (node->str_value_ == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    node->str_len_ = length;
  }
  if (type == T_BOOL) node->is_literal_bool_ = true;
  if (type == T_SORT_ASC || type == T_SORT_DESC) node->value_ = 2;
  if (type == T_IDENT) {
    node->is_input_quoted_ = raw_length > 0 && (*raw == '`' || *raw == '"');
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
  if (type == T_OP_LIKE && source.text == "not like") {
    ParseNode *negation = new_node(pool, T_OP_NOT, 1);
    if (negation == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
    negation->children_[0] = node;
    node = negation;
  }
  return common::OB_SUCCESS;
}

int build_tree(const common::ObString &sql, const peg::SelectTree &tree, ParseResult &result)
{
  ParseNode *query = nullptr;
  const int ret = build_node(sql, tree.root, tree.parameters, result.malloc_pool_, query);
  if (ret != common::OB_SUCCESS) return ret;
  ParseNode *root = new_node(result.malloc_pool_, T_STMT_LIST, 1);
  if (root == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
  root->children_[0] = query;
  result.result_tree_ = root;
  result.question_mark_ctx_.count_ = tree.parameters;
  result.question_mark_ctx_.by_ordinal_ = tree.parameters != 0;
  if (result.is_dynamic_sql_) {
    MEMCPY(result.no_param_sql_, sql.ptr(), sql.length());
    result.no_param_sql_[sql.length()] = '\0';
    result.no_param_sql_len_ = sql.length();
  }
  return common::OB_SUCCESS;
}
}

bool is_rust_select(const common::ObString &sql)
{
  int64_t offset = 0;
  while (offset < sql.length() && std::isspace(static_cast<unsigned char>(sql.ptr()[offset]))) ++offset;
  if (sql.length() - offset < 6 || STRNCASECMP(sql.ptr() + offset, "select", 6) != 0) return false;
  offset += 6;
  while (offset < sql.length() && std::isspace(static_cast<unsigned char>(sql.ptr()[offset]))) ++offset;
  return common::ObString(sql.length() - offset, sql.ptr() + offset).prefix_match("/*+parser_mvp*/");
}

int parse_rust_select(const common::ObString &sql, ParseResult &result)
{
  static thread_local char error[MAX_ERROR_MSG];
  error[0] = '\0';
  result.error_msg_ = error;
  result.input_sql_ = sql.ptr();
  result.input_sql_len_ = sql.length();
  auto parsed = peg::parse_select(rust::Slice<const uint8_t>(
      reinterpret_cast<const uint8_t *>(sql.ptr()), sql.length()),
      result.sql_mode_ & SMO_ANSI_QUOTES, result.sql_mode_ & SMO_NO_BACKSLASH_ESCAPES,
      result.sql_mode_ & SMO_PIPES_AS_CONCAT, result.sql_mode_ & SMO_HIGH_NOT_PRECEDENCE);
  if (!parsed.error.empty()) {
    snprintf(error, sizeof(error), "rust-peg: %s", parsed.error.c_str());
    result.start_col_ = parsed.error_offset + 1;
    result.end_col_ = parsed.error_offset + 1;
    int line = 1;
    for (size_t i = 0; i < parsed.error_offset; ++i) line += sql.ptr()[i] == '\n';
    LOG_USER_ERROR(OB_ERR_PARSE_SQL, error, 0, "", line);
    return common::OB_ERR_PARSE_SQL;
  }
  return build_tree(sql, parsed, result);
}
}
