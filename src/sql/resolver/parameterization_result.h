/*
 * Copyright (c) 2026 OceanBase.
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

#pragma once

#include <functional>
#include "lib/hash/ob_hashmap.h"
#include "sql/parser/parse_node.h"

namespace oceanbase::sql
{
struct NumericLiteral;

struct SyntaxIdentityHash
{
  int operator()(const ParseNode *node, uint64_t &hash) const
  {
    hash = reinterpret_cast<uintptr_t>(node);
    return common::OB_SUCCESS;
  }
};

enum class ParameterOrigin { Literal, UnaryNegation, Subtraction };

struct NumericLowering
{
  enum class Kind { FoldUnaryExpression, NegateLiteral, AddNegatedOperand };
  Kind kind;
  const NumericLiteral *literal;
};

struct ParameterBinding
{
  int64_t parameter_index;
  int64_t raw_token_index;
  ParameterOrigin origin;
};

class ParameterizationResult
{
public:
  int bind(const ParseNode *node, int64_t parameter_index, int64_t raw_token_index,
           ParameterOrigin origin = ParameterOrigin::Literal)
  {
    int ret = common::OB_SUCCESS;
    if (!bindings_.created()) {
      ret = bindings_.create(1, "SqlParamBind");
    }
    if (OB_SUCC(ret)) {
      ret = bindings_.set_refactored(node, ParameterBinding{parameter_index, raw_token_index, origin}, 1);
    }
    return ret;
  }

  const ParameterBinding *find(const ParseNode *node) const
  {
    return bindings_.created() ? bindings_.get(node) : nullptr;
  }

  const NumericLowering *numeric_lowering(const ParseNode *node) const
  {
    return numeric_lowerings_.created() ? numeric_lowerings_.get(node) : nullptr;
  }

  int fold_negation(const ParseNode *node, const NumericLiteral *literal)
  {
    return lower_numeric(node, {NumericLowering::Kind::FoldUnaryExpression, literal});
  }

  int negate_literal(const ParseNode *node, const NumericLiteral *literal)
  {
    return lower_numeric(node, {NumericLowering::Kind::NegateLiteral, literal});
  }

  int add_negated_operand(const ParseNode *node)
  {
    return lower_numeric(node, {NumericLowering::Kind::AddNegatedOperand, nullptr});
  }

  void reset()
  {
    if (bindings_.created()) bindings_.clear();
    if (numeric_lowerings_.created()) numeric_lowerings_.clear();
  }

private:
  int lower_numeric(const ParseNode *node, const NumericLowering &lowering)
  {
    int ret = common::OB_SUCCESS;
    if (!numeric_lowerings_.created()) ret = numeric_lowerings_.create(1, "SqlNumeric");
    if (OB_SUCC(ret)) ret = numeric_lowerings_.set_refactored(node, lowering, 1);
    return ret;
  }

  template <typename Value>
  using SyntaxMap = common::hash::ObHashMap<const ParseNode *, Value, common::hash::NoPthreadDefendMode,
      SyntaxIdentityHash, std::equal_to<const ParseNode *>,
      common::hash::SimpleAllocer<typename common::hash::HashMapTypes<const ParseNode *, Value>::AllocType>,
      common::hash::NormalPointer, common::ObMalloc, 2>;
  SyntaxMap<ParameterBinding> bindings_;
  SyntaxMap<NumericLowering> numeric_lowerings_;
};
}
