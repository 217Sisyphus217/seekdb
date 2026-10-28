/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_PC
#include "rust/config.h"
#include <algorithm>
#include "sql/parser/parsed_sql.h"
#include "ob_sql_parameterization.h"
#include "lib/json/ob_json_print_utils.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/expr/numeric_literal.h"
#include "sql/resolver/expr/literal_decode_options.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
using namespace share::schema;

SqlInfo::SqlInfo() :
  total_(0),
  last_active_time_(0),
  hit_count_(0),
  ps_need_parameterized_(true),
  need_check_fp_(false)
{
}

namespace oceanbase::sql {
namespace {
using SyntaxKind = peg::NodeKind;

struct ParameterScope {
  bool excluded = false;
  bool projection = false;
  bool direct_value = false;
  bool arithmetic = false;
  bool ignore_scale = false;
  bool positive = false;
  const peg::Node *subtraction = nullptr;
  const peg::Node *subtraction_operand = nullptr;
};

struct LiteralOccurrence {
  const peg::Node *literal;
  const peg::Node *expression;
  ParameterScope scope;
  ParameterOrigin origin;
  size_t source_begin;
};

class SyntaxParameterizer {
public:
  SyntaxParameterizer(ObIAllocator &allocator, const ObSQLSessionInfo &session,
                      const ParsedSql &parsed, SqlInfo &info, ParamStore &params,
                      ParameterizationResult &bindings, SQL_EXECUTION_MODE mode,
                      bool outline, bool from_pl)
      : allocator_(allocator), session_(session), parsed_(parsed), info_(info),
        params_(params), bindings_(bindings), mode_(mode), outline_(outline), from_pl_(from_pl) {}

  int collect(const peg::Node &node, ParameterScope scope = {});
  int apply(const ObIArray<ObPCParam *> &tokens, SelectItemParamInfoMap *projection_names);
  const ObIArray<ObString> &user_variables() const { return user_variables_; }

private:
  int query(const peg::Node &node, ParameterScope scope);
  int table(const peg::Node &node, ParameterScope scope);
  int window(const peg::Node &node, ParameterScope scope);
  int call(const peg::Node &node, ParameterScope scope);
  int list(rust::Slice<const peg::Node> nodes, ParameterScope scope);
  int exclude(const peg::Node &node);
  int literal(const peg::Node &node, const peg::Node &expression,
              ParameterScope scope, ParameterOrigin origin = ParameterOrigin::Literal);
  int negate(const peg::Node &node, bool negate, const NumericLiteral *&result);
  void subtraction_scope(const peg::Node &node, ParameterScope &scope);
  int projection_name(const peg::Node &projection, const ObIArray<ObPCParam *> &tokens,
                      SelectItemParamInfoMap &names);
  int bind(LiteralOccurrence &occurrence, int64_t token_index,
           const ObPCParam &token, ObCollationType collation);
  bool excluded(size_t start, size_t end) const;

  ObIAllocator &allocator_;
  const ObSQLSessionInfo &session_;
  const ParsedSql &parsed_;
  SqlInfo &info_;
  ParamStore &params_;
  ParameterizationResult &bindings_;
  SQL_EXECUTION_MODE mode_;
  bool outline_;
  bool from_pl_;
  ObSEArray<LiteralOccurrence, 32> literals_;
  ObSEArray<const peg::Node *, 16> exclusions_;
  ObSEArray<const peg::Node *, 16> projections_;
  ObSEArray<ObString, 4> user_variables_;
};

int SyntaxParameterizer::exclude(const peg::Node &node)
{
  int ret = OB_SUCCESS;
  info_.ps_need_parameterized_ = false;
  ParameterScope scope;
  scope.excluded = true;
  if (node.kind == SyntaxKind::JsonColumn
      && peg::json_column_kind(node) == peg::JsonColumnKind::Nested) {
    OZ (exclude(peg::json_path(node)));
    for (const auto &column : peg::json_nested_columns(node)) OZ (exclude(column));
    return ret;
  }
  for (const auto &reference : peg::syntax_literals(node)) {
    OZ (literal(reference.node, reference.node, scope));
  }
  if (node.kind == SyntaxKind::JsonColumn
      && peg::json_column_kind(node) == peg::JsonColumnKind::Value) {
    OZ (exclude(peg::json_error_clause(node)));
    OZ (exclude(peg::json_empty_clause(node)));
  }
  if (node.start < node.end) { OZ (exclusions_.push_back(&node)); }
  return ret;
}

bool SyntaxParameterizer::excluded(size_t start, size_t end) const
{
  for (const peg::Node *node : exclusions_) {
    if (node->start <= start && end <= node->end) return true;
  }
  return false;
}

int SyntaxParameterizer::literal(const peg::Node &node, const peg::Node &expression,
                                 ParameterScope scope, ParameterOrigin origin)
{
  const auto value = peg::expression_literal(node);
  if (value.hidden) return OB_SUCCESS;
  if (scope.excluded || !value.charset.empty()) info_.ps_need_parameterized_ = false;
  if (scope.projection && value.kind != peg::LiteralKind::Placeholder) {
    info_.ps_need_parameterized_ = false;
  }
  const size_t source_begin = scope.subtraction_operand == &node
      ? peg::binary_left(*scope.subtraction).end : expression.start;
  return literals_.push_back({&node, &expression, scope, origin, source_begin});
}

int SyntaxParameterizer::list(rust::Slice<const peg::Node> nodes, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  for (const auto &node : nodes) {
    if (OB_FAIL(collect(node, scope))) break;
  }
  return ret;
}

int SyntaxParameterizer::query(const peg::Node &node, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
  const auto excluded_scope = [&] { auto value = scope; value.excluded = true; return value; }();
  for (const auto &cte : peg::query_ctes(node)) {
    OZ (collect(peg::cte_query(cte), scope));
  }
  if (node.kind == SyntaxKind::Select || node.kind == SyntaxKind::PlSubquery) {
    OZ (list(peg::query_set_operands(node), scope));
    for (const auto &projection : peg::query_projection(node)) {
      OZ (projections_.push_back(&projection));
      auto projection_scope = scope;
      projection_scope.projection = true;
      OZ (collect(peg::projection_expression(projection), projection_scope));
      if (peg::projection_alias_form(projection) == peg::AliasForm::String) {
        OZ (exclude(peg::projection_alias(projection)));
      }
    }
    if (peg::query_has_grouping(node)) {
      for (const auto &key : peg::grouping_keys(peg::query_grouping(node))) {
        OZ (collect(peg::sort_expression(key), excluded_scope));
      }
    }
    if (peg::query_has_having(node)) OZ (collect(peg::query_having(node), scope));
    for (const auto &named : peg::query_windows(node)) OZ (exclude(named));
  } else if (node.kind == SyntaxKind::Insert || node.kind == SyntaxKind::Replace) {
    auto value_scope = scope;
    value_scope.direct_value = true;
    if (peg::insert_source(node) == peg::InsertSource::Query) {
      OZ (collect(peg::insert_query(node), scope));
    } else if (peg::insert_source(node) == peg::InsertSource::Rows) {
      for (const auto &row : peg::insert_rows(node)) OZ (list(peg::row_elements(row), value_scope));
    } else {
      for (const auto &assignment : peg::insert_assignments(node)) {
        OZ (collect(peg::assignment_value(assignment), value_scope));
      }
    }
    for (const auto &assignment : peg::insert_on_duplicate(node)) {
      OZ (collect(peg::assignment_value(assignment), value_scope));
    }
  } else if (node.kind == SyntaxKind::Update) {
    auto value_scope = scope;
    value_scope.direct_value = true;
    for (const auto &assignment : peg::query_assignments(node)) {
      OZ (collect(peg::assignment_value(assignment), value_scope));
    }
  }
  for (const auto &source : peg::query_sources(node)) OZ (table(source, scope));
  if (peg::query_has_filter(node)) OZ (collect(peg::query_filter(node), scope));
  const auto order = peg::query_order(node);
  bool vector_order = false;
  if (order.size() == 1) {
    const auto &expression = peg::sort_expression(order[0]);
    if (expression.kind == SyntaxKind::Function) {
      const auto spelling = peg::call_name(expression);
      const ObString name(spelling.size(), spelling.data());
      vector_order = name.case_compare("l2_distance") == 0
          || name.case_compare("negative_inner_product") == 0
          || name.case_compare("cosine_distance") == 0
          || name.case_compare("semantic_distance") == 0
          || name.case_compare("semantic_vector_distance") == 0;
    }
  }
  for (const auto &key : order) {
    OZ (collect(peg::sort_expression(key), vector_order ? scope : excluded_scope));
  }
  if (peg::query_has_limit(node)) {
    const auto &limit = peg::query_limit(node);
    auto limit_scope = scope;
    limit_scope.positive = true;
    OZ (collect(peg::limit_count(limit), limit_scope));
    if (peg::limit_has_offset(limit)) OZ (collect(peg::limit_offset(limit), limit_scope));
  }
  if (peg::query_has_lock(node)) OZ (exclude(peg::query_lock(node)));
  for (const auto &parameter : peg::query_vector_parameters(node)) OZ (exclude(parameter));
  return ret;
}

int SyntaxParameterizer::table(const peg::Node &node, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  if (node.kind == SyntaxKind::Table) {
    const auto &source = peg::table_source(node);
    if (source.kind != SyntaxKind::Relation) OZ (collect(source, scope));
    if (peg::table_has_sample(node)) OZ (exclude(peg::table_sample(node)));
    if (peg::table_has_snapshot(node)) OZ (collect(peg::table_snapshot(node), scope));
  } else if (node.kind == SyntaxKind::JoinedTable) {
    OZ (table(peg::join_left(node), scope));
    OZ (table(peg::join_right(node), scope));
    if (peg::join_condition(node) == peg::JoinCondition::On) {
      OZ (collect(peg::join_predicate(node), scope));
    }
  } else if (node.kind == SyntaxKind::JsonTable) {
    OZ (collect(peg::json_document(node), scope));
    OZ (exclude(peg::json_path(node)));
    for (const auto &column : peg::json_table_columns(node)) OZ (exclude(column));
  } else if (node.kind == SyntaxKind::TableCollection) {
    OZ (collect(peg::collection_table_expression(node), scope));
  } else if (node.kind == SyntaxKind::Select) {
    OZ (query(node, scope));
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int SyntaxParameterizer::window(const peg::Node &node, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  OZ (list(peg::window_partition(node), scope));
  auto order_scope = scope;
  order_scope.excluded = true;
  for (const auto &key : peg::window_order(node)) OZ (collect(peg::sort_expression(key), order_scope));
  if (peg::window_has_frame(node)) {
    const auto &frame = peg::window_frame(node);
    const auto bound = [&](const peg::Node &value) {
      const auto kind = peg::frame_bound(value);
      return kind == peg::FrameBound::Preceding || kind == peg::FrameBound::Following
          ? collect(peg::frame_bound_offset(value), scope) : OB_SUCCESS;
    };
    OZ (bound(peg::frame_start(frame)));
    if (peg::frame_has_end(frame)) OZ (bound(peg::frame_end(frame)));
  }
  return ret;
}

int SyntaxParameterizer::call(const peg::Node &node, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  const auto args = peg::call_arguments(node);
  const auto spelling = peg::call_name(node);
  const ObString name(spelling.size(), spelling.data());
  int64_t excluded_begin = args.size();
  int64_t excluded_end = args.size();
  if ((name.case_compare("userenv") == 0 || name.case_compare("unix_timestamp") == 0) && args.size() == 1) {
    excluded_begin = 0;
  } else if (name.case_compare("substr") == 0 && args.size() == 3) {
    excluded_begin = 1;
  } else if (name.case_compare("weight_string") == 0 && args.size() == 5) {
    excluded_begin = 1;
  } else if (args.size() == 2) {
    if (name.case_compare("convert") == 0 || name.case_compare("char") == 0
        || name.case_compare("str_to_date") == 0 || name.case_compare("date_format") == 0
        || name.case_compare("from_unixtime") == 0 || name.case_compare("round") == 0
        || name.case_compare("left") == 0 || name.case_compare("substr") == 0
        || name.case_compare("truncate") == 0) {
      excluded_begin = 1;
    } else if (name.case_compare("name_const") == 0 || name.case_compare("json_schema_valid") == 0
               || name.case_compare("json_schema_validation_report") == 0) {
      excluded_begin = 0;
      excluded_end = 1;
    } else if (name.case_compare("json_overlaps") == 0 || name.case_compare("json_extract") == 0
               || name.case_compare("json_member_of") == 0) {
      excluded_begin = 0;
    }
  }
  if (node.kind == SyntaxKind::StringConcat || name.case_compare("json_equal") == 0
      || name.case_compare("json_member_of") == 0 || name.case_compare("json_contains") == 0
      || excluded_begin != excluded_end) info_.ps_need_parameterized_ = false;
  scope.direct_value = scope.arithmetic = false;
  switch (node.kind) {
    case SyntaxKind::ArrayMap: case SyntaxKind::ArrayFilter:
    case SyntaxKind::ArrayFirst: case SyntaxKind::ArraySortBy:
      OZ (collect(peg::collection_lambda(node), scope));
      break;
    default: break;
  }
  for (size_t i = 0; OB_SUCC(ret) && i < args.size(); ++i) {
    auto argument_scope = scope;
    argument_scope.excluded |= i >= excluded_begin && i < excluded_end;
    argument_scope.ignore_scale = node.kind == SyntaxKind::Point || name.case_compare("st_point") == 0;
    switch (node.kind) {
      case SyntaxKind::CurrentTimestamp: case SyntaxKind::CurrentTime: case SyntaxKind::CurrentDate:
      case SyntaxKind::UtcTimestamp: case SyntaxKind::UtcTime: case SyntaxKind::Sysdate:
        argument_scope.excluded = true;
        break;
      default: break;
    }
    OZ (collect(args[i], argument_scope));
  }
  for (const auto &key : peg::aggregate_order(node)) {
    auto order_scope = scope;
    order_scope.excluded = true;
    OZ (collect(peg::sort_expression(key), order_scope));
  }
  if (peg::aggregate_has_separator(node)) OZ (collect(peg::aggregate_separator(node), scope));
  return ret;
}

int SyntaxParameterizer::collect(const peg::Node &node, ParameterScope scope)
{
  int ret = OB_SUCCESS;
  if (scope.excluded) info_.ps_need_parameterized_ = false;
  if (peg::is_literal(node)) return literal(node, node, scope);
  switch (node.kind) {
    case SyntaxKind::Select: case SyntaxKind::PlSubquery: case SyntaxKind::Insert: case SyntaxKind::Replace:
    case SyntaxKind::Update: case SyntaxKind::Delete:
      return query(node, scope);
    case SyntaxKind::PlCall:
      return list(peg::routine_call_arguments(node), scope);
    case SyntaxKind::PlNamedArgument:
      return collect(peg::named_argument_value(node), scope);
    case SyntaxKind::Explain:
      return collect(peg::explain_statement(node), scope);
    case SyntaxKind::Ident: case SyntaxKind::Column: case SyntaxKind::Star:
    case SyntaxKind::Relation: case SyntaxKind::Default:
      break;
    case SyntaxKind::Parenthesized:
      return collect(peg::parenthesized_expression(node), scope);
    case SyntaxKind::UserVariable: case SyntaxKind::GetUserVariable: {
      const auto name = peg::variable_name(node);
      OZ (user_variables_.push_back(ObString(name.size(), name.data())));
      break;
    }
    case SyntaxKind::SystemVariable:
      break;
    case SyntaxKind::UserAssignment:
      return collect(peg::variable_assignment_value(node), scope);
    case SyntaxKind::Unary: {
      const auto &operand = peg::unary_operand(node);
      scope.direct_value = false;
      scope.ignore_scale = false;
      if (!scope.excluded && peg::unary_operator(node) == peg::UnaryOperator::Minus
          && peg::is_literal(operand)) {
        const auto value = peg::expression_literal(operand);
        if (value.kind == peg::LiteralKind::Integer || value.kind == peg::LiteralKind::Unsigned
            || value.kind == peg::LiteralKind::Decimal || value.kind == peg::LiteralKind::Float) {
          OZ (literal(operand, node, scope, ParameterOrigin::UnaryNegation));
        } else {
          scope.arithmetic = false;
          OZ (literal(operand, operand, scope, value.kind == peg::LiteralKind::String
              ? ParameterOrigin::UnaryNegation : ParameterOrigin::Literal));
        }
      } else {
        scope.arithmetic = false;
        const peg::Node *value = &operand;
        while (value->kind == SyntaxKind::Parenthesized) {
          value = &peg::parenthesized_expression(*value);
        }
        if (peg::unary_operator(node) == peg::UnaryOperator::Minus
            && value->kind == SyntaxKind::Uint) {
          scope.excluded = true;
        }
        OZ (collect(operand, scope));
      }
      break;
    }
    case SyntaxKind::Binary: {
      const auto op = peg::binary_operator(node);
      scope.direct_value = scope.ignore_scale = false;
      scope.arithmetic = op == peg::BinaryOperator::Add || op == peg::BinaryOperator::Subtract
          || op == peg::BinaryOperator::Multiply || op == peg::BinaryOperator::Divide;
      OZ (collect(peg::binary_left(node), scope));
      if (peg::like_has_escape(node)) {
        scope.excluded = true;
        info_.ps_need_parameterized_ = false;
      }
      subtraction_scope(node, scope);
      OZ (collect(peg::binary_right(node), scope));
      if (peg::like_has_escape(node)) OZ (collect(peg::like_escape(node), scope));
      break;
    }
    case SyntaxKind::In:
      scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
      OZ (collect(peg::predicate_subject(node), scope));
      if (peg::membership_source(node) == peg::MembershipSource::Query) {
        OZ (collect(peg::membership_query(node), scope));
      } else {
        OZ (list(peg::membership_expressions(node), scope));
      }
      break;
    case SyntaxKind::Between:
      scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
      OZ (collect(peg::predicate_subject(node), scope));
      OZ (collect(peg::between_lower(node), scope));
      OZ (collect(peg::between_upper(node), scope));
      break;
    case SyntaxKind::Is:
      scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
      OZ (collect(peg::predicate_subject(node), scope));
      scope.excluded = true;
      OZ (collect(peg::is_test(node), scope));
      break;
    case SyntaxKind::All: case SyntaxKind::Any:
      OZ (collect(peg::quantified_query(node), scope));
      break;
    case SyntaxKind::Exists:
      OZ (collect(peg::exists_query(node), scope));
      break;
    case SyntaxKind::ExprList: case SyntaxKind::ValueVector:
      OZ (list(peg::row_elements(node), scope));
      break;
    case SyntaxKind::Case:
      scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
      if (peg::case_has_operand(node)) OZ (collect(peg::case_operand(node), scope));
      for (const auto &arm : peg::case_arms(node)) {
        OZ (collect(peg::case_condition(arm), scope));
        OZ (collect(peg::case_result(arm), scope));
      }
      if (peg::case_has_else(node)) OZ (collect(peg::case_else(node), scope));
      break;
    case SyntaxKind::Cast:
      scope.direct_value = scope.arithmetic = scope.ignore_scale = false;
      OZ (collect(peg::cast_argument(node), scope));
      OZ (exclude(peg::cast_target(node)));
      break;
    case SyntaxKind::SetCollation:
      OZ (collect(peg::collation_argument(node), scope));
      OZ (exclude(peg::collation_specification(node)));
      break;
    case SyntaxKind::WindowFunction:
      OZ (collect(peg::window_function(node), scope));
      OZ (window(peg::window_definition(node), scope));
      break;
    case SyntaxKind::Lambda:
      OZ (collect(peg::lambda_body(node), scope));
      break;
    case SyntaxKind::JsonValue: case SyntaxKind::JsonQuery:
      OZ (collect(peg::json_document(node), scope));
      OZ (exclude(peg::json_path(node)));
      OZ (exclude(peg::json_returning(node)));
      OZ (exclude(peg::json_error_clause(node)));
      OZ (exclude(peg::json_empty_clause(node)));
      if (node.kind == SyntaxKind::JsonQuery) OZ (exclude(peg::json_mismatch_clause(node)));
      if (peg::json_options(node).on_error == peg::JsonResponse::Default) {
        OZ (exclude(peg::json_error_default(node)));
      }
      if (peg::json_options(node).on_empty == peg::JsonResponse::Default) {
        OZ (exclude(peg::json_empty_default(node)));
      }
      break;
    case SyntaxKind::MatchAgainst:
      OZ (collect(peg::fulltext_search(node), scope));
      break;
    case SyntaxKind::Table: case SyntaxKind::JoinedTable: case SyntaxKind::JsonTable:
    case SyntaxKind::TableCollection:
      OZ (table(node, scope));
      break;
    default:
      if (peg::is_call_expression(node)) return call(node, scope);
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported syntax in parameterization", "kind", static_cast<int>(node.kind), K(ret));
  }
  return ret;
}

int SyntaxParameterizer::negate(const peg::Node &node, bool negative, const NumericLiteral *&result)
{
  const auto value = peg::expression_literal(node);
  NumericKind kind;
  switch (value.kind) {
    case peg::LiteralKind::Integer: kind = NumericKind::Integer; break;
    case peg::LiteralKind::Unsigned:
      kind = negative ? (value.integer == (uint64_t(1) << 63) ? NumericKind::Integer : NumericKind::Decimal)
                      : NumericKind::Unsigned;
      break;
    case peg::LiteralKind::Decimal: kind = NumericKind::Decimal; break;
    case peg::LiteralKind::Float: kind = NumericKind::Double; break;
    default: return OB_ERR_UNEXPECTED;
  }
  ObString text(value.text.size(), value.text.data());
  uint64_t integer = value.integer;
  if (negative) {
    char *spelling = static_cast<char *>(allocator_.alloc(text.length() + 2));
    if (spelling == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
    spelling[0] = '-';
    MEMCPY(spelling + 1, text.ptr(), text.length());
    spelling[text.length() + 1] = '\0';
    text.assign_ptr(spelling, text.length() + 1);
    if (kind == NumericKind::Integer) integer = uint64_t(0) - integer;
  }
  void *storage = allocator_.alloc(sizeof(NumericLiteral));
  if (storage == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
  result = new (storage) NumericLiteral{kind, integer, text};
  return OB_SUCCESS;
}

void SyntaxParameterizer::subtraction_scope(const peg::Node &node, ParameterScope &scope)
{
  if (peg::binary_operator(node) == peg::BinaryOperator::Subtract) {
    const peg::Node *operand = &peg::binary_right(node);
    while (operand->kind == SyntaxKind::Binary) {
      const auto op = peg::binary_operator(*operand);
      if (op != peg::BinaryOperator::Multiply && op != peg::BinaryOperator::Divide
          && op != peg::BinaryOperator::IntegerDivide && op != peg::BinaryOperator::Modulo) break;
      operand = &peg::binary_left(*operand);
    }
    if (peg::is_literal(*operand)) {
      const auto value = peg::expression_literal(*operand);
      if (value.kind == peg::LiteralKind::Integer || value.kind == peg::LiteralKind::Unsigned
          || value.kind == peg::LiteralKind::Decimal || value.kind == peg::LiteralKind::Float) {
        scope.subtraction = &node;
        scope.subtraction_operand = operand;
      }
    }
  }
}

int SyntaxParameterizer::bind(LiteralOccurrence &occurrence, int64_t token_index,
                              const ObPCParam &token, ObCollationType collation)
{
  int ret = OB_SUCCESS;
  const auto value_syntax = peg::expression_literal(*occurrence.literal);
  const bool placeholder = value_syntax.kind == peg::LiteralKind::Placeholder;
  const int64_t placeholder_index = placeholder ? peg::placeholder_index(*occurrence.literal) : -1;
  const bool execute = ObSqlParameterization::is_execute_mode(mode_);
  if (occurrence.origin == ParameterOrigin::UnaryNegation
      && occurrence.expression->kind == SyntaxKind::Unary) {
    if (token.source_offset_ < occurrence.literal->start) {
      const NumericLiteral *negative = nullptr;
      OZ (negate(*occurrence.literal, true, negative));
      OZ (bindings_.fold_negation(occurrence.expression, negative));
    } else {
      occurrence.expression = occurrence.literal;
      occurrence.origin = ParameterOrigin::Literal;
      occurrence.scope.arithmetic = false;
    }
  }
  if (occurrence.scope.subtraction_operand == occurrence.literal
      && token.source_offset_ < occurrence.literal->start) {
    const NumericLiteral *negative = nullptr;
    OZ (negate(*occurrence.literal, !from_pl_, negative));
    OZ (bindings_.negate_literal(occurrence.literal, negative));
    OZ (bindings_.add_negated_operand(occurrence.scope.subtraction));
  }
  const auto *lowering = bindings_.numeric_lowering(occurrence.expression);
  ParameterOrigin origin = occurrence.origin;
  if (lowering != nullptr && lowering->kind == NumericLowering::Kind::NegateLiteral) {
    origin = ParameterOrigin::Subtraction;
  }
  bool decimal_int = false;
  bool mysql_dates = false;
  OZ (ObSQLUtils::check_enable_decimalint(&session_, decimal_int));
  OZ (ObSQLUtils::check_enable_mysql_compatible_dates(&session_, false, mysql_dates));
  const bool arithmetic = occurrence.scope.arithmetic && value_syntax.kind == peg::LiteralKind::Integer;
  const NumericLiteralOptions numeric{stmt::T_NONE, decimal_int, from_pl_, arithmetic,
                                      session_.get_min_const_integer_precision()};
  const LiteralDecodeOptions decode{numeric, collation, session_.get_timezone_info(), session_.get_sql_mode(),
                                    share::COMPAT_MYSQL57, mysql_dates, outline_, nullptr, NumericSignMode::Source};
  ObObjParam value;
  ObString prefix;
  if (OB_SUCC(ret)) {
    if (lowering != nullptr && lowering->literal != nullptr) {
      OZ (ObResolverUtils::resolve_numeric_literal(*lowering->literal, numeric, allocator_, value, prefix));
    } else if (placeholder && !ObSqlParameterization::is_prepare_mode(mode_) && !execute
               && token.literal_->kind != peg::LiteralKind::Placeholder) {
      OZ (ObResolverUtils::resolve_literal(*token.literal_, decode, allocator_, value, prefix));
    } else {
      OZ (ObResolverUtils::resolve_expression_literal(*occurrence.literal, decode, allocator_, value, prefix));
    }
  }
  if (origin == ParameterOrigin::UnaryNegation) OZ (info_.neg_param_index_.add_member(token_index));
  if (origin == ParameterOrigin::Subtraction) OZ (info_.trans_from_minus_index_.add_member(token_index));
  if (arithmetic) OZ (info_.fmt_int_or_ch_decint_idx_.add_member(token_index));
  if (occurrence.scope.positive && (value.is_numeric_type() || placeholder)) {
    OZ (info_.must_be_positive_index_.add_member(token_index));
  }
  if (placeholder) {
    if (occurrence.scope.direct_value) {
      OZ (info_.no_check_type_offsets_.push_back(placeholder_index));
    } else {
      OZ (info_.need_check_type_param_offsets_.add_member(placeholder_index));
    }
  } else {
    value.set_need_to_check_type(!occurrence.scope.direct_value);
    if (occurrence.scope.ignore_scale && !value.is_decimal_int()) value.set_ignore_scale_check(true);
    OZ (info_.fixed_param_idx_.push_back(params_.count()));
  }
  value.set_is_boolean(value.is_boolean() || value_syntax.kind == peg::LiteralKind::Boolean);
  value.set_raw_text_info(token.source_offset_, token.literal_->source.length());
  if (!execute) {
    OZ (bindings_.bind(occurrence.expression, params_.count(), token_index, origin));
    OZ (params_.push_back(value));
  }
  return ret;
}

int SyntaxParameterizer::apply(const ObIArray<ObPCParam *> &tokens, SelectItemParamInfoMap *projection_names)
{
  int ret = OB_SUCCESS;
  ObCollationType collation = CS_TYPE_INVALID;
  OZ (session_.get_collation_connection(collation));
  std::sort(literals_.begin(), literals_.end(), [](const auto &left, const auto &right) {
    return left.literal->start < right.literal->start;
  });
  size_t occurrence_index = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
    const auto &token = *tokens.at(i);
    const size_t start = token.source_offset_;
    const size_t end = start + token.literal_->source.length();
    while (occurrence_index < literals_.count() && literals_.at(occurrence_index).literal->end <= start) {
      ++occurrence_index;
    }
    LiteralOccurrence *occurrence = nullptr;
    if (occurrence_index < literals_.count()) {
      auto &candidate = literals_.at(occurrence_index);
      if (candidate.source_begin <= start && end <= candidate.literal->end) occurrence = &candidate;
    }
    if (occurrence == nullptr && !excluded(start, end)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("scanner literal has no syntax occurrence", K(ret), K(start), K(end), K(i));
    } else {
      ObCharsetType charset = CHARSET_INVALID;
      if (occurrence != nullptr) {
        const auto value = peg::expression_literal(*occurrence->literal);
        if (!value.charset.empty()) {
          const ObString name(value.charset.size(), value.charset.data());
          charset = ObCharset::charset_type(name);
          if (charset == CHARSET_INVALID) {
            ret = OB_ERR_UNKNOWN_CHARSET;
            LOG_USER_ERROR(OB_ERR_UNKNOWN_CHARSET, name.length(), name.ptr());
          }
        }
      }
      OZ (info_.param_charset_type_.push_back(charset));
      const bool merged_subtraction = occurrence != nullptr
          && occurrence->scope.subtraction_operand == occurrence->literal
          && start < occurrence->literal->start;
      const bool preserve_subtraction = merged_subtraction
          && (&peg::binary_right(*occurrence->scope.subtraction) != occurrence->literal
              || occurrence->literal->kind == SyntaxKind::Uint
              || is_no_unsigned_subtraction(session_.get_sql_mode()));
      const bool unsigned_negation = occurrence != nullptr
          && occurrence->origin == ParameterOrigin::UnaryNegation
          && occurrence->expression->kind == SyntaxKind::Unary
          && occurrence->literal->kind == SyntaxKind::Uint
          && start >= occurrence->literal->start;
      if (occurrence == nullptr || occurrence->scope.excluded || preserve_subtraction || unsigned_negation) {
        info_.ps_need_parameterized_ = false;
        OZ (info_.not_param_index_.add_member(i));
        if (occurrence != nullptr && occurrence->literal->kind == SyntaxKind::Parameter) {
          OZ (info_.ps_not_param_offsets_.push_back(peg::placeholder_index(*occurrence->literal)));
        }
      } else {
        OZ (bind(*occurrence, i, token, collation));
      }
      if (info_.need_check_fp_) {
        ObPCParseInfo parse_info;
        parse_info.param_idx_ = i;
        parse_info.flag_ = info_.not_param_index_.has_member(i) ? NOT_PARAM : NORMAL_PARAM;
        parse_info.raw_text_pos_ = start;
        OZ (info_.parse_infos_.push_back(parse_info));
      }
    }
  }
  info_.total_ = tokens.count();
  if (OB_SUCC(ret) && outline_) {
    for (const auto &occurrence : literals_) {
      if (occurrence.literal->kind == SyntaxKind::Parameter && occurrence.scope.excluded) {
        ret = OB_INVALID_OUTLINE;
        LOG_USER_ERROR(OB_INVALID_OUTLINE, "? appears at invalid position in sql_text");
        break;
      }
    }
  }
  if (OB_SUCC(ret) && projection_names != nullptr) {
    if (!projection_names->created() && !projections_.empty()) {
      OZ (projection_names->create(projections_.count(), "ProjectionNames"));
    }
    for (const auto *projection : projections_) {
      OZ (projection_name(*projection, tokens, *projection_names));
    }
  }
  return ret;
}

int SyntaxParameterizer::projection_name(const peg::Node &projection,
                                         const ObIArray<ObPCParam *> &tokens,
                                         SelectItemParamInfoMap &names)
{
  int ret = OB_SUCCESS;
  if (peg::projection_alias_form(projection) != peg::AliasForm::Absent) return ret;
  const auto &expression = peg::projection_expression(projection);
  if (expression.kind == SyntaxKind::Star
      || (expression.kind == SyntaxKind::Column && peg::name_is_wildcard(expression))) return ret;
  const peg::Node *display = &expression;
  while (display->kind == SyntaxKind::Parenthesized) display = &peg::parenthesized_expression(*display);
  bool string_literal = false;
  ObString original_name = parsed_.source(expression);
  if (peg::is_literal(*display)) {
    const auto value = peg::expression_literal(*display);
    string_literal = value.kind == peg::LiteralKind::String;
    if (string_literal) {
      original_name.assign_ptr(value.text.data(), value.concatenated ? value.first_part_length : value.text.size());
    }
  }
  SelectItemParamInfo info;
  int64_t expression_position = expression.start;
  const int64_t buffer_size = SelectItemParamInfo::PARAMED_FIELD_BUF_LEN;
  for (const auto &occurrence : literals_) {
    const auto *binding = bindings_.find(occurrence.expression);
    if (binding == nullptr || occurrence.literal->start < expression.start || occurrence.literal->end > expression.end) continue;
    if (info.name_len_ >= buffer_size) break;
    const auto &token = *tokens.at(binding->raw_token_index);
    const int64_t source_position = token.source_offset_;
    const int64_t prefix_length = std::min(buffer_size - info.name_len_, source_position - expression_position);
    if (!string_literal && prefix_length > 0) {
      MEMCPY(info.paramed_field_name_ + info.name_len_, original_name.ptr() + expression_position - expression.start,
             prefix_length);
      info.name_len_ += prefix_length;
    }
    expression_position = source_position + token.literal_->source.length();
    OZ (info.questions_pos_.push_back(info.name_len_));
    OZ (info.params_idx_.push_back(binding->raw_token_index));
    if (OB_SUCC(ret) && info.name_len_ < buffer_size) info.paramed_field_name_[info.name_len_++] = '?';
    if (binding->origin == ParameterOrigin::UnaryNegation) OZ (info.neg_params_idx_.add_member(binding->raw_token_index));
  }
  if (!string_literal) {
    const int64_t offset = expression_position - expression.start;
    const int64_t remaining = std::min(original_name.length() - offset, buffer_size - info.name_len_);
    if (remaining > 0) {
      MEMCPY(info.paramed_field_name_ + info.name_len_, original_name.ptr() + offset, remaining);
      info.name_len_ += remaining;
    }
  }
  info.esc_str_flag_ = string_literal && bindings_.find(display) != nullptr;
  if (info.params_idx_.count() == 1 && info.name_len_ == 1 && info.paramed_field_name_[0] == '?') {
    info.null_literal_ = tokens.at(info.params_idx_.at(0))->literal_->kind == peg::LiteralKind::Null;
  }
  if (OB_SUCC(ret) && info.params_idx_.count() > 0) OZ (names.set_refactored(&projection, info));
  return ret;
}
}
}

int ObSqlParameterization::transform_syntax_tree(ObIAllocator &allocator,
                                                const ObSQLSessionInfo &session,
                                                const ObIArray<ObPCParam *> *raw_params,
                                                const ParsedSql &parsed,
                                                SqlInfo &sql_info,
                                                ParamStore &params,
                                                ParameterizationResult &bindings,
                                                SelectItemParamInfoMap *select_item_param_infos,
                                                bool is_transform_outline,
                                                SQL_EXECUTION_MODE execution_mode,
                                                bool is_from_pl,
                                                ObIArray<ObString> *user_variables)
{
  int ret = OB_SUCCESS;
  bindings.reset();
  if (parsed.statement() == nullptr) return OB_ERR_UNEXPECTED;
  ObFastParserResult scan;
  if (raw_params == nullptr) {
    FPContext context(session.get_charsets4parser());
    context.sql_mode_ = session.get_sql_mode();
    OZ (fast_parser(allocator, context, parsed.original_source(), scan));
    raw_params = &scan.raw_params_;
  }
  SyntaxParameterizer parameterizer(allocator, session, parsed, sql_info, params, bindings,
                                    execution_mode, is_transform_outline, is_from_pl);
  OZ (parameterizer.collect(*parsed.statement()));
  OZ (parameterizer.apply(*raw_params, select_item_param_infos));
  if (user_variables != nullptr) OZ (append(*user_variables, parameterizer.user_variables()));
  return ret;
}

SQL_EXECUTION_MODE ObSqlParameterization::get_sql_execution_mode(ObPlanCacheCtx &pc_ctx)
{
  if (PC_PS_MODE == pc_ctx.mode_ || PC_PL_MODE == pc_ctx.mode_) {
    return pc_ctx.is_parameterized_execute_
        ? (PC_PL_MODE == pc_ctx.mode_ ? PL_EXECUTE_MODE : PS_EXECUTE_MODE)
        : (PC_PL_MODE == pc_ctx.mode_ ? PL_PREPARE_MODE : PS_PREPARE_MODE);
  }
  return TEXT_MODE;
}

bool ObSqlParameterization::is_prepare_mode(SQL_EXECUTION_MODE mode)
{
  return mode == PS_PREPARE_MODE || mode == PL_PREPARE_MODE;
}

bool ObSqlParameterization::is_execute_mode(SQL_EXECUTION_MODE mode)
{
  return mode == PS_EXECUTE_MODE || mode == PL_EXECUTE_MODE;
}

bool ObSqlParameterization::is_text_mode(SQL_EXECUTION_MODE mode)
{
  return mode == TEXT_MODE;
}

int ObSqlParameterization::check_and_generate_param_info(const ObIArray<ObPCParam *> &raw_params,
                                                         const SqlInfo &sql_info,
                                                         ObIArray<ObPCParam *> &special_params)
{
  int ret = OB_SUCCESS;
  if (sql_info.total_ != raw_params.count()) {
    ret = OB_NOT_SUPPORTED;
    if (sql_info.need_check_fp_) {
      SQL_PC_LOG(ERROR, "const number of fast parse and normal parse is different",
                "fast_parse_const_num", raw_params.count(),
                "normal_parse_const_num", sql_info.total_);
    }
  }

  if (OB_FAIL(ret)) {

  } else {
    ObPCParam *pc_param = NULL;
    for (int32_t i = 0; OB_SUCC(ret) && i < raw_params.count(); i ++) {
      // not param and neg param cannot be the same param, it has been guaranteed during transform_tree
      pc_param = raw_params.at(i);
      if (OB_ISNULL(pc_param)) {
        ret = OB_INVALID_ARGUMENT;
        SQL_PC_LOG(WARN, "invalid argument", K(ret));
      } else if (OB_ISNULL(pc_param->literal_)) {
        ret = OB_INVALID_ARGUMENT;
        SQL_PC_LOG(WARN, "invalid argument", K(pc_param->literal_), K(ret));
      } else if (sql_info.not_param_index_.has_member(i)) { //not param
        pc_param->flag_ = NOT_PARAM;
        if (OB_FAIL(special_params.push_back(pc_param))) {
        }
      } else if (sql_info.neg_param_index_.has_member(i)) {//neg param
        // If it is T_VARCHAR then there is no need to record as a negative number, ?sql also does not need to be merged-?
        if (peg::LiteralKind::String == pc_param->literal_->kind) {
          //do nothing
        } else {
          pc_param->flag_ = NEG_PARAM;
          if (OB_FAIL(special_params.push_back(pc_param))) {
          }
        }
      } else if (sql_info.trans_from_minus_index_.has_member(i)) {
        pc_param->flag_ = TRANS_NEG_PARAM;
        if (OB_FAIL(special_params.push_back(pc_param))) {
        }
      } else {
        pc_param->flag_ = NORMAL_PARAM;
      }
    } // for end

    if (sql_info.need_check_fp_) {
      // do nothing
      int last_pos = -1;
      int last_param_idx = -1;

      for (int i = 0; OB_SUCC(ret) && i < sql_info.parse_infos_.count(); i++) {
        if (last_pos > sql_info.parse_infos_.at(i).raw_text_pos_ ||
            last_param_idx > sql_info.parse_infos_.at(i).param_idx_) {
          ret = OB_NOT_SUPPORTED;
          SQL_PC_LOG(ERROR, "invalid parse order", K(last_param_idx), K(last_pos),
                                                   K(sql_info.parse_infos_.at(i).raw_text_pos_),
                                                   K(sql_info.parse_infos_.at(i).param_idx_),
                                                   K(sql_info.parse_infos_), K(ret));
        } else {
          last_pos = sql_info.parse_infos_.at(i).raw_text_pos_;
          last_param_idx = sql_info.parse_infos_.at(i).param_idx_;
        }
      }
    }
  }

  return ret;
}
// Long path SQL parameterization
int ObSqlParameterization::parameterize_syntax_tree(common::ObIAllocator &allocator,
                                                    bool is_transform_outline,
                                                    ObPlanCacheCtx &pc_ctx,
                                                    const ParsedSql &parsed,
                                                    ParamStore &params,
                                                    ObCharsets4Parser charsets4parser)
{
  int ret = OB_SUCCESS;
  if (pc_ctx.select_item_param_infos_.created()) {
    pc_ctx.select_item_param_infos_.clear();
  }
  SqlInfo sql_info;
  bool need_parameterized = false;
  SQL_EXECUTION_MODE mode = get_sql_execution_mode(pc_ctx);
  ObSQLSessionInfo *session = NULL;
  ObSEArray<ObPCParam *, OB_PC_SPECIAL_PARAM_COUNT> special_params;
  ObSEArray<ObString, 4> user_var_names;
  FPContext fp_ctx(charsets4parser);

  int tmp_ret = OB_SUCCESS;
  tmp_ret = OB_E(EventTable::EN_SQL_PARAM_FP_NP_NOT_SAME_ERROR) OB_SUCCESS;
  if (OB_SUCCESS != tmp_ret) {
    sql_info.need_check_fp_ = true;
  }
  int64_t reserved_cnt = 0;
  if (OB_ISNULL(session = pc_ctx.exec_ctx_.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    SQL_PC_LOG(ERROR, "got session is NULL", K(ret));
  } else {
    fp_ctx.enable_batched_multi_stmt_ = pc_ctx.sql_ctx_.handle_batched_multi_stmt();
    fp_ctx.sql_mode_ = session->get_sql_mode();
    fp_ctx.is_format_ = false;
  }

  if (OB_FAIL(ret)) {
  } else if (is_prepare_mode(mode)
            || is_transform_outline) {
    // if so, faster parser is needed
    // otherwise, fast parser has been done before
    pc_ctx.fp_result_.reset();

    ObString raw_sql = pc_ctx.raw_sql_;
    if (pc_ctx.sql_ctx_.is_do_insert_batch_opt()) {
      raw_sql = pc_ctx.insert_batch_opt_info_.new_reconstruct_sql_;
    } else if (pc_ctx.exec_ctx_.has_dynamic_values_table()) {
      raw_sql = pc_ctx.new_raw_sql_;
    }
    if (OB_FAIL(fast_parser(allocator,
                            fp_ctx,
                            raw_sql,
                            pc_ctx.fp_result_))) {
    }
  }

  if (OB_FAIL(ret)) {
    // do nothing
  } else if (FALSE_IT(reserved_cnt = pc_ctx.fp_result_.raw_params_.count())) {
  } else if (!is_execute_mode(mode)
             && (OB_FAIL(params.reserve(reserved_cnt)) // Reserve array to avoid extending
             || OB_FAIL(sql_info.param_charset_type_.reserve(reserved_cnt))
             || OB_FAIL(sql_info.fixed_param_idx_.reserve(reserved_cnt)))) {
  } else if (OB_FAIL(transform_syntax_tree(allocator,
                                           *session,
                                           is_execute_mode(mode) ? NULL : &pc_ctx.fp_result_.raw_params_,
                                           parsed,
                                           sql_info,
                                           params,
                                           pc_ctx.parameterization_,
                                           is_prepare_mode(mode) ? NULL : &pc_ctx.select_item_param_infos_,
                                           is_transform_outline,
                                           mode, false, &user_var_names))) {
    if (OB_NOT_SUPPORTED != ret) {
      SQL_PC_LOG(WARN, "fail to normal parameterized parser tree", K(ret));
    }
  } else {
    need_parameterized = (!(PC_PS_MODE == pc_ctx.mode_ || PC_PL_MODE == pc_ctx.mode_)
                          || (is_prepare_mode(mode) && sql_info.ps_need_parameterized_));
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(pc_ctx.sql_ctx_.set_related_user_var_names(user_var_names, allocator))) {
  } else if (is_execute_mode(mode)) {
    if (OB_FAIL(gen_ps_not_param_var(sql_info.ps_not_param_offsets_, params, pc_ctx))) {
    } else if (OB_FAIL(construct_no_check_type_params(sql_info.no_check_type_offsets_,
                                                      sql_info.need_check_type_param_offsets_,
                                                      params))) {
    }
  } else if (need_parameterized) {
    if (OB_FAIL(check_and_generate_param_info(pc_ctx.fp_result_.raw_params_,
                                              sql_info,
                                              special_params))) {
      if (OB_NOT_SUPPORTED != ret) {
        SQL_PC_LOG(WARN, "fail to check and generate param info", K(ret));
      } else if (sql_info.need_check_fp_) {
        SQL_PC_LOG(INFO, "print tree", K(session->get_current_query_string()), "sql", parsed.original_source());
      } else {
        // do nothing
      }
    } else if (OB_FAIL(gen_special_param_info(sql_info, pc_ctx))) {
    } else {
      // do nothing
    }
    if (OB_SUCC(ret)) {
      int32_t pos = 0;
      ObString con_str;
      int64_t len = pc_ctx.raw_sql_.length();
      char* buf = (char *)allocator.alloc(len);

      if (NULL == buf) {
        SQL_PC_LOG(WARN, "fail to alloc buf", K(pc_ctx.raw_sql_.length()));
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(construct_sql(pc_ctx.fp_result_.pc_key_.name_, special_params, buf, len, pos))) {
      } else if (!pc_ctx.is_batch_insert_opt_ &&
                 !pc_ctx.exec_ctx_.has_dynamic_values_table() &&
                 OB_FAIL(ObSqlParameterization::formalize_sql_text(allocator, pc_ctx.raw_sql_,
                                                  pc_ctx.sql_ctx_.plan_key_.format_sql_,
                                                  sql_info, fp_ctx))) {
        SQL_PC_LOG(WARN, "fail to formalize sql text", K(ret), K(pc_ctx.raw_sql_));
      } else {
        pc_ctx.sql_ctx_.plan_key_.constructed_sql_.assign_ptr(buf, pos);
        pc_ctx.ps_need_parameterized_ = sql_info.ps_need_parameterized_;
        pc_ctx.normal_parse_const_cnt_ = sql_info.total_;
      }
    }
  }
  if (OB_FAIL(ret)) {
    pc_ctx.parameterization_.reset();
    if (pc_ctx.select_item_param_infos_.created()) pc_ctx.select_item_param_infos_.clear();
  }

  return ret;
}
// Generate not param info information and index information
int ObSqlParameterization::gen_special_param_info(SqlInfo &sql_info, ObPlanCacheCtx &pc_ctx)
{
  int ret = OB_SUCCESS;
  const LiteralToken *raw_param = NULL;
  NotParamInfo np_info;
  pc_ctx.not_param_info_.set_capacity(sql_info.not_param_index_.num_members());
  for (int32_t i = 0; OB_SUCC(ret) && i < pc_ctx.fp_result_.raw_params_.count(); i ++) {
    if (OB_ISNULL(pc_ctx.fp_result_.raw_params_.at(i))) {
      ret = OB_INVALID_ARGUMENT;
    } else if (NULL == (raw_param = pc_ctx.fp_result_.raw_params_.at(i)->literal_)) {
      ret = OB_INVALID_ARGUMENT;
      SQL_PC_LOG(WARN, "invalid argument", K(ret));
    } else if (sql_info.not_param_index_.has_member(i)) { //not param
      np_info.reset();
      np_info.idx_ = i;
      np_info.raw_text_ = raw_param->source;
      if (OB_FAIL(pc_ctx.not_param_info_.push_back(np_info))) {
      }
    }
  } // for end

  if (OB_SUCC(ret)) {
    if (OB_FAIL(pc_ctx.not_param_index_.add_members2(sql_info.not_param_index_))) {
    } else if (OB_FAIL(pc_ctx.neg_param_index_.add_members2(sql_info.neg_param_index_))) {
    } else if (OB_FAIL(pc_ctx.neg_param_index_.add_members2(sql_info.trans_from_minus_index_))) {
    } else if (OB_FAIL(pc_ctx.param_charset_type_.assign(sql_info.param_charset_type_))) {
    } else if (OB_FAIL(pc_ctx.fixed_param_idx_.assign(sql_info.fixed_param_idx_))) {
    } else if (OB_FAIL(pc_ctx.must_be_positive_index_.add_members2(sql_info.must_be_positive_index_))) {
    } else if (OB_FAIL(pc_ctx.fmt_int_or_ch_decint_idx_.add_members2(sql_info.fmt_int_or_ch_decint_idx_))){
    }
  }

  return ret;
}

int ObSqlParameterization::gen_ps_not_param_var(const ObIArray<int64_t> &offsets,
                                                ParamStore &params,
                                                ObPlanCacheCtx &pc_ctx)
{
  int ret = OB_SUCCESS;
  pc_ctx.not_param_var_.set_capacity(offsets.count());
  for (int i = 0; OB_SUCC(ret) && i < offsets.count(); ++i) {
    const int64_t offset = offsets.at(i);
    PsNotParamInfo ps_not_param_var;
    ps_not_param_var.idx_ = offset;
    if (offset >= params.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ps_not_param_var.ps_param_ = params.at(offset);
      if (OB_FAIL(pc_ctx.not_param_var_.push_back(ps_not_param_var))) {
      } else if (OB_FAIL(pc_ctx.not_param_index_.add_member(offset))) {
      } 
    }
  }
  return ret;
}

int ObSqlParameterization::construct_no_check_type_params(const ObIArray<int64_t> &no_check_type_offsets,
                                                          const ObBitSet<> &need_check_type_offsets,
                                                          ParamStore &params)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < no_check_type_offsets.count(); i++) {
    const int64_t offset = no_check_type_offsets.at(i);
    if (offset < 0 || offset >= params.count()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (need_check_type_offsets.has_member(offset)) {
      // do nothing
    } else if (!params.at(offset).is_ext() && !ob_is_enumset_inner_tc(params.at(offset).get_meta().get_type())) { // extend type and enum or set inner type need to be checked
      params.at(offset).set_need_to_check_type(false);
    } else {
      // real type do not need to be checked
      params.at(offset).set_need_to_check_extend_type(false);
    }
  } // for end

  return ret;
}

int ObSqlParameterization::construct_not_param(const ObString &no_param_sql,
                                                ObPCParam *pc_param,
                                                char *buf,
                                                int32_t buf_len,
                                                int32_t &pos,
                                                int32_t &idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(pc_param)) {
    ret = OB_INVALID_ARGUMENT;
    SQL_PC_LOG(WARN, "invalid argument", K(ret));
  } else {
    int32_t len = (int32_t)pc_param->sql_position_ - idx;
    if (len > buf_len - pos) {
      ret = OB_BUF_NOT_ENOUGH;
      LOG_WARN("buffer not enough", K(pos), K(buf_len), K(len), K(pc_param->sql_position_), K(idx));
    } else if (len > 0) {
      //copy text
      MEMCPY(buf + pos, no_param_sql.ptr() + idx, len);
      idx = (int32_t)pc_param->sql_position_ + 1;
      pos += len;
      //copy raw param
      MEMCPY(buf + pos, pc_param->literal_->source.ptr(), pc_param->literal_->source.length());
      pos += (int32_t)pc_param->literal_->source.length();
    }
  }
  return ret;
}

int ObSqlParameterization::construct_neg_param(const ObString &no_param_sql,
                                                ObPCParam *pc_param,
                                                char *buf,
                                                int32_t buf_len,
                                                int32_t &pos,
                                                int32_t &idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(pc_param)) {
    ret = OB_INVALID_ARGUMENT;
    SQL_PC_LOG(WARN, "invalid argument", K(ret));
  } else {
    int32_t len = (int32_t)pc_param->sql_position_ - idx;
    if (len > buf_len - pos) {
      ret = OB_BUF_NOT_ENOUGH;
      LOG_WARN("buffer not enough", K(pos), K(buf_len), K(len), K(pc_param->sql_position_), K(idx));
    } else if (len > 0) {
      MEMCPY(buf + pos, no_param_sql.ptr() + idx, len);
      pos += len;
    }
    if (OB_SUCC(ret)) {
      idx = (int32_t)pc_param->sql_position_ + 1;
      buf[pos++] = '?';
    }
  }
  return ret;
}

int ObSqlParameterization::construct_trans_neg_param(const ObString &no_param_sql,
                                                      ObPCParam *pc_param,
                                                      char *buf,
                                                      int32_t buf_len,
                                                      int32_t &pos,
                                                      int32_t &idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(pc_param)) {
    ret = OB_INVALID_ARGUMENT;
    SQL_PC_LOG(WARN, "invalid argument", K(ret));
  } else {
    int32_t len = (int32_t)pc_param->sql_position_ - idx;
    if (len > buf_len - pos) {
      ret = OB_BUF_NOT_ENOUGH;
      LOG_WARN("buffer not enough", K(pos), K(buf_len), K(len), K(pc_param->sql_position_), K(idx));
    } else if (len > 0) {
      MEMCPY(buf + pos, no_param_sql.ptr() + idx, len);
      pos += len;
    }
    if (OB_SUCC(ret)) {
      // for 'select * from t where a -   1 = 2', the statement is 'select * from t where a -   ? = ?'
      // so we need fill spaces between '-' and '?', so here it is
      buf[pos++] = '-';
      int64_t tmp_pos = 0;
      for (; tmp_pos < pc_param->literal_->text.length() && isspace(pc_param->literal_->text.ptr()[tmp_pos]); tmp_pos++);
      if (OB_UNLIKELY(tmp_pos >= pc_param->literal_->text.length())) {
        int ret = OB_ERR_UNEXPECTED;
      } else {
        if ('-' != pc_param->literal_->text.ptr()[tmp_pos]) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("expected neg sign here", K(tmp_pos), K(ObString(pc_param->literal_->text.length(),
                                                                    pc_param->literal_->text.ptr())));
        } else {
          tmp_pos += 1;
          for (; tmp_pos < pc_param->literal_->text.length() && isspace(pc_param->literal_->text.ptr()[tmp_pos]); tmp_pos++) {
            buf[pos++] = ' ';
          }
        }
        if (OB_SUCC(ret)) {
          buf[pos++] = '?';
          idx = pc_param->sql_position_ + 1;
        }
      }
    }
  }
  return ret;
}

int ObSqlParameterization::construct_sql(const ObString &no_param_sql,
                                         ObIArray<ObPCParam *> &pc_params,
                                         char *buf,
                                         int32_t buf_len,
                                         int32_t &pos) // stored length
{
  int ret = OB_SUCCESS;
  int32_t idx = 0; // original offset position with ?sql
  ObPCParam *pc_param = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < pc_params.count(); i ++) {
    pc_param = pc_params.at(i);
    int32_t len = 0; // length of the text to be copied
    if (OB_ISNULL(pc_param)) {
      ret = OB_INVALID_ARGUMENT;
      SQL_PC_LOG(WARN, "invalid argument", K(ret));
    } else if (NOT_PARAM == pc_param->flag_) {
      OZ (construct_not_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
    } else if (NEG_PARAM == pc_param->flag_) {
      OZ (construct_neg_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
    } else if (TRANS_NEG_PARAM == pc_param->flag_) {
      OZ (construct_trans_neg_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
    } else {
      //do nothing
    }
  } //for end

  if (OB_SUCCESS == ret) {
    int32_t len = no_param_sql.length() - idx;
    if (len > buf_len - pos) {
      ret = OB_BUF_NOT_ENOUGH;
      LOG_WARN("buffer not enough", K(pos), K(buf_len), K(len), K(no_param_sql.length()), K(idx));
    } else if (len > 0) {
      MEMCPY(buf + pos, no_param_sql.ptr() + idx, len);
      idx += len;
      pos += len;
    }
  }
  return ret;
}

int ObSqlParameterization::try_format_in_expr(const common::ObString &con_sql,
                                              char *buf,
                                              int32_t buf_len,
                                              int32_t &pos,
                                              bool& can_format)
{
  int ret = OB_SUCCESS;
  can_format = false;
  int64_t in_pos = 0;
  int64_t in_end = 0;
  int64_t qm_cnt = 0;
  bool need_break = false;
  int64_t old_str_pos = 0;
  // find in pos
  MEMSET(buf, 0x00, buf_len);
  if (con_sql.empty()) {
    // do nothing
  } else {
    while (!need_break) {
      bool found = false;
      int old_in_pos = in_pos;
      if (OB_FAIL(search_in_expr_pos(con_sql.ptr(), con_sql.length(), in_pos, found))) {
      } else if (!found) {
        need_break = true;
        in_pos = con_sql.length();
      } else if (OB_FAIL(search_vector(con_sql.ptr(), con_sql.length(), in_pos, in_end, can_format, qm_cnt))) {
      } else {
        // do nothing
      }
      if (OB_FAIL(ret)) {
        // do nothing
      } else if (can_format) {
        int64_t old_pos = pos;
        MEMCPY(buf + pos, con_sql.ptr() + old_in_pos, (in_pos - old_in_pos));
        pos += (in_pos - old_in_pos);
        if (found) {
          buf[pos] = '(';
          pos++;
          if (qm_cnt > 1) {
            buf[pos] = '(';
            pos++;
          }
          buf[pos] = '.';
          buf[pos + 1] = '.';
          buf[pos + 2] = '.';
          pos += 3;
          buf[pos] = ')';
          pos++;
          if (qm_cnt > 1) {
            buf[pos] = ')';
            pos++;
          }
        }
        in_pos = in_end;
        old_str_pos = in_end;
      } else {
        MEMCPY(buf + pos, con_sql.ptr() + old_str_pos, (in_pos - old_str_pos));
        pos += (in_pos - old_str_pos);
        old_str_pos = in_pos;
      }
    }
  }
  // search vector
  return ret;
}


bool ObSqlParameterization::is_in_expr_prefix(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

int ObSqlParameterization::search_in_expr_pos(const char* buf, const int64_t buf_len, int64_t& pos, bool& found) 
{
  int ret = OB_SUCCESS;
  found = false;
  for (int64_t i = pos; !found && i < buf_len; i++) {
    if (i + 3 < buf_len && is_in_expr_prefix(buf[i])
        && (buf[i + 1] == 'i' || buf[i + 1] == 'I')
        && (buf[i + 2] == 'n' || buf[i + 2] == 'N')
        && (is_in_expr_prefix(buf[i + 3]) || buf[i + 3] == '(')) {
      pos = i + 4;
      if (buf[i + 3] == '(') pos--;
      found = true;
    }
  }
  return ret;
}

int ObSqlParameterization::search_vector(const char* buf,
                                         const int64_t buf_len,
                                         int64_t& vec_start,
                                         int64_t& vec_end,
                                         bool &is_valid,
                                         int64_t& qm_cnt)
{
  int ret = OB_SUCCESS;
  bool need_break = false;
  int vec_level = 0;
  is_valid = false;
  qm_cnt = 1;
  for (int64_t i = vec_start; !need_break  && i <  buf_len; i++) {
    if (buf[i] == ' ') {
      // skip
    } else {
      if (buf[i] == '(') {
        vec_level++;
        if (vec_level == 2) {
          qm_cnt = 0;
        }
      } else {
        if (vec_level > 0 && vec_level <= 2) {
          if (buf[i] == ')') {
            vec_level--;
            if (vec_level == 0) { 
              vec_end = i + 1;
              need_break = true;
              is_valid = true;
            }
          } else if (buf[i] == '?') {
            // skip
            if (vec_level == 2) {
              qm_cnt++;
            }
          } else if (buf[i] == ',') {
            // skip
          } else {
            // invalid character, break
            need_break = true;
            vec_end = buf_len;
            is_valid = false;
          }
        } else {
          need_break = true;
          vec_end = buf_len;
          is_valid = false;
        }
      }
    }

  }
  return ret;
}

int ObSqlParameterization::construct_sql_for_pl(const ObString &no_param_sql,
                                                ObIArray<ObPCParam *> &pc_params,
                                                char *buf,
                                                int32_t buf_len,
                                                int32_t &pos) // stored length
{
  int ret = OB_SUCCESS;
  int32_t idx = 0; // original offset position with ?sql
  ObPCParam *pc_param = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < pc_params.count(); i ++) {
    pc_param = pc_params.at(i);
    int32_t len = 0; // length of the text to be copied
    if (OB_ISNULL(pc_param)) {
      ret = OB_INVALID_ARGUMENT;
      SQL_PC_LOG(WARN, "invalid argument", K(ret));
    } else if (NOT_PARAM == pc_param->flag_) {
      int32_t len = (int32_t)pc_param->sql_position_ - idx;
      if (0 == len) {
        MEMCPY(buf + pos, pc_param->literal_->source.ptr(), pc_param->literal_->source.length());
        pos += (int32_t)pc_param->literal_->source.length();
        idx = (int32_t)pc_param->sql_position_ + 1;
      } else {
        OZ (construct_not_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
      }
    } else if (NEG_PARAM == pc_param->flag_) {
      OZ (construct_neg_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
    } else if (TRANS_NEG_PARAM == pc_param->flag_) {
      OZ (construct_trans_neg_param(no_param_sql, pc_param, buf, buf_len, pos, idx));
    } else {
      //do nothing
    }
  } //for end

  if (OB_SUCCESS == ret) {
    int32_t len = no_param_sql.length() - idx;
    if (len > buf_len - pos) {
      ret = OB_BUF_NOT_ENOUGH;
    } else if (len > 0) {
      MEMCPY(buf + pos, no_param_sql.ptr() + idx, len);
      idx += len;
      pos += len;
    }
  }
  return ret;
}

bool ObSqlParameterization::need_fast_parser(const ObString &sql)
{
  bool b_ret = true;
  const char *stmt = sql.ptr();
  int64_t len = sql.length();
  int64_t leading_space_len = 0;
  while (leading_space_len < len && isspace(stmt[leading_space_len])) {
    ++leading_space_len;
  }
  len -= leading_space_len;
  stmt += leading_space_len;
  if (len > 4 && ('s' == stmt[0] || 'S' == stmt[0]) && ('h' == stmt[1] || 'H' == stmt[1])) {
    if (0 == STRNCASECMP(stmt, "show", 4)) {
      b_ret = false;
    }
  }
  return b_ret;
}

int ObSqlParameterization::formalize_fast_parameter_sql(ObIAllocator &allocator,
                                          const ObString &src_sql,
                                          ObString &dest_sql,
                                          ObIArray<ObPCParam *> &raw_params,
                                          const FPContext &fp_ctx)
{
  int ret = OB_SUCCESS;
  bool is_call_procedure = false;
  const bool is_contain_select = src_sql.length() > 6 && 0 == STRNCASECMP(src_sql.ptr(), "select", 6);
  if (is_contain_select || (need_fast_parser(src_sql)
      && (!ObParser::is_pl_stmt(src_sql, nullptr, &is_call_procedure) || is_call_procedure))) {
    FPContext context = fp_ctx;
    context.is_format_ = true;
    ObFastParserResult result;
    if (OB_FAIL(ObFastParser::parse(src_sql, context, allocator, result))) {
    } else {
      dest_sql = result.pc_key_.name_;
      ret = raw_params.assign(result.raw_params_);
    }
  }
  return ret;
}

int ObSqlParameterization::formalize_sql_filter_hint(ObIAllocator &allocator,
                                                      const ObString &src_sql,
                                                      ObString &dest_sql,
                                                      ObIArray<ObPCParam *> &raw_params)
{
  int ret = OB_SUCCESS;
  bool is_call_procedure = false;
  const bool is_contain_select = src_sql.length() > 6 && 0 == STRNCASECMP(src_sql.ptr(), "select", 6);
  if (is_contain_select || (need_fast_parser(src_sql)
      && (!ObParser::is_pl_stmt(src_sql, nullptr, &is_call_procedure) || is_call_procedure))) {
    FPContext context;
    context.is_format_ = true;
    ObFastParserResult result;
    if (OB_FAIL(ObFastParser::parse(src_sql, context, allocator, result))) {
    } else if (result.raw_params_.count() != raw_params.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      dest_sql = result.pc_key_.name_;
      for (int64_t i = 0; i < raw_params.count(); ++i) {
        if (raw_params.at(i) != nullptr) {
          raw_params.at(i)->sql_position_ = result.raw_params_.at(i)->sql_position_;
        }
      }
    }
  }
  return ret;
}

int ObSqlParameterization::fast_parser(ObIAllocator &allocator,
                                       const FPContext &fp_ctx,
                                       const ObString &sql,
                                       ObFastParserResult &fp_result)
{
  bool is_call_procedure = false;
  const bool is_contain_select = sql.length() > 6 && 0 == STRNCASECMP(sql.ptr(), "select", 6);
  if (!is_contain_select && (!need_fast_parser(sql)
      || (ObParser::is_pl_stmt(sql, nullptr, &is_call_procedure) && !is_call_procedure))) {
    fp_result.pc_key_.name_ = sql;
    return OB_SUCCESS;
  }
  return ObFastParser::parse(sql, fp_ctx, allocator, fp_result);
}

int ObSqlParameterization::raw_fast_parameterize_sql(ObIAllocator &allocator,
                                                     const ObSQLSessionInfo &session,
                                                     const ObString &sql,
                                                     ObString &no_param_sql,
                                                     ObIArray<ObPCParam *> &raw_params,
                                                     ParseMode parse_mode)
{
  int ret = OB_SUCCESS;
  int64_t length = sql.length();
  while (length > 0 && SPACE_FLAGS[static_cast<uint8_t>(sql[length - 1])]) --length;
  if (length > 0 && sql[length - 1] == '\0') --length;
  while (length > 0 && SPACE_FLAGS[static_cast<uint8_t>(sql[length - 1])]) --length;
  FPContext context(session.get_charsets4parser());
  context.sql_mode_ = session.get_sql_mode();
  context.parameterize_ = parse_mode == FP_MODE || parse_mode == FP_PARAMERIZE_AND_FILTER_HINT_MODE;
  context.ignore_hint_ = parse_mode == FP_PARAMERIZE_AND_FILTER_HINT_MODE
      || parse_mode == FP_NO_PARAMERIZE_AND_FILTER_HINT_MODE;
  ObFastParserResult result;
  NG_TRACE(pc_fast_parse_start);
  if (length == 0) {
    ret = OB_ERR_EMPTY_QUERY;
  } else if (OB_FAIL(ObFastParser::parse(ObString(length, sql.ptr()), context, allocator, result))) {
    if (ret == OB_ERR_PARSER_SYNTAX) ret = OB_ERR_PARSE_SQL;
  } else {
    no_param_sql = result.pc_key_.name_;
    ret = raw_params.assign(result.raw_params_);
  }
  NG_TRACE(pc_fast_parse_end);
  return ret;
}

int ObSqlParameterization::formalize_sql_text(ObIAllocator &allocator, const ObString &src_sql, 
                                              ObString &fmt_sql, const SqlInfo &sql_info,
                                              const FPContext &fp_ctx)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObPCParam *, OB_PC_RAW_PARAM_COUNT> fmt_raw_params;
  ObSEArray<ObPCParam *, OB_PC_SPECIAL_PARAM_COUNT> fmt_special_params;
  bool can_format = true;
  int64_t format_len = src_sql.length() * 2;
  int32_t pos = 0;
  int32_t format_pos = 0;
  char* buf = (char *)allocator.alloc(format_len);
  char* buf_format = (char *)allocator.alloc(format_len);

  if ((NULL == buf || NULL == buf_format)) {
    SQL_PC_LOG(WARN, "fail to alloc buf", K(src_sql.length()));
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObSqlParameterization::formalize_fast_parameter_sql(allocator,
                                                src_sql, fmt_sql, fmt_raw_params, fp_ctx))) {
  } else if (OB_FAIL(check_and_generate_param_info(fmt_raw_params,
                                                  sql_info,
                                                  fmt_special_params))) {
    if (OB_NOT_SUPPORTED != ret) {
      SQL_PC_LOG(WARN, "fail to check and generate param info", K(ret));
    } else {
      // do nothing
    }
  } else if (OB_FAIL(construct_sql(fmt_sql, fmt_special_params, buf, format_len, pos))) {
  } else if (OB_FAIL(try_format_in_expr(ObString(pos, buf), buf_format, format_len, format_pos, can_format))) {
  } else {
    fmt_sql.assign_ptr(buf_format, format_pos);
  }
  return ret;
}
