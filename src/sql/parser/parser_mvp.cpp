#define USING_LOG_PREFIX SQL_PARSER
#include "rust/parser_mvp.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/parse_malloc.h"
#include "lib/string/ob_string.h"
#include "share/ob_errno.h"
#include "lib/oblog/ob_log.h"

namespace oceanbase::sql {
using common::OB_ERR_PARSE_SQL;
namespace {
bool set_text(ParseNode &node, const rust::String &text, void *pool)
{
  node.str_value_ = parse_strndup(text.data(), text.size(), pool);
  node.str_len_ = text.size();
  return node.str_value_ != nullptr;
}
}

int parse_mvp_select(const common::ObString &sql, ParseResult &result)
{
  static thread_local char error[MAX_ERROR_MSG];
  error[0] = '\0';
  result.error_msg_ = error;
  result.input_sql_ = sql.ptr();
  result.input_sql_len_ = sql.length();
  // A request marker keeps the prototype out of normal and internal SQL traffic.
  auto parsed = peg::parse_integer_select(rust::Slice<const uint8_t>(
      reinterpret_cast<const uint8_t *>(sql.ptr()), sql.length()));
  if (!parsed.error.empty()) {
    snprintf(error, sizeof(error), "%s", parsed.error.c_str());
    LOG_USER_ERROR(OB_ERR_PARSE_SQL, error, 0, "", 1);
    return common::OB_ERR_PARSE_SQL;
  }
  void *pool = result.malloc_pool_;
  ParseNode *select = new_node(pool, T_SELECT, PARSE_SELECT_MAX_IDX);
  ParseNode *list = new_node(pool, T_PROJECT_LIST, parsed.columns.size());
  ParseNode *root = new_node(pool, T_STMT_LIST, 1);
  if (!select || !list || !root) {
    return common::OB_ALLOCATE_MEMORY_FAILED;
  }
  select->value_ = 0;
  root->children_[0] = select;
  select->children_[PARSE_SELECT_SELECT] = list;
  for (size_t i = 0; i < parsed.columns.size(); ++i) {
    const auto &column = parsed.columns[i];
    ParseNode *value = new_node(pool, T_INT, 0);
    ParseNode *projection = new_node(pool, T_PROJECT_STRING, 1);
    if (!value || !projection || !set_text(*value, column.literal, pool)
        || !set_text(*projection, column.expression, pool)) {
      return common::OB_ALLOCATE_MEMORY_FAILED;
    }
    value->value_ = column.value;
    value->param_num_ = 1;
    value->raw_text_ = value->str_value_;
    value->text_len_ = value->str_len_;
    projection->raw_sql_offset_ = column.offset;
    projection->children_[0] = value;
    list->children_[i] = projection;
    if (!column.alias.empty()) {
      ParseNode *name = new_node(pool, T_IDENT, 0);
      ParseNode *alias = new_node(pool, T_ALIAS, 2);
      if (!name || !alias || !set_text(*name, column.alias, pool)
          || !set_text(*alias, column.alias, pool)) {
        return common::OB_ALLOCATE_MEMORY_FAILED;
      }
      alias->children_[0] = value;
      alias->children_[1] = name;
      projection->children_[0] = alias;
    }
  }
  result.result_tree_ = root;
  return common::OB_SUCCESS;
}
}
