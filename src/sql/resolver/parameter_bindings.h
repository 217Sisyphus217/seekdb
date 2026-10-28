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
struct SyntaxIdentityHash
{
  int operator()(const ParseNode *node, uint64_t &hash) const
  {
    hash = reinterpret_cast<uintptr_t>(node);
    return common::OB_SUCCESS;
  }
};

struct ParameterBinding
{
  int64_t parameter_index;
  int64_t raw_token_index;
};

class ParameterBindings
{
public:
  int bind(const ParseNode *node, int64_t parameter_index, int64_t raw_token_index)
  {
    int ret = common::OB_SUCCESS;
    if (!bindings_.created()) {
      ret = bindings_.create(1, "SqlParamBind");
    }
    if (OB_SUCC(ret)) {
      ret = bindings_.set_refactored(node, ParameterBinding{parameter_index, raw_token_index}, 1);
    }
    return ret;
  }

  const ParameterBinding *find(const ParseNode *node) const
  {
    return bindings_.created() ? bindings_.get(node) : nullptr;
  }

  void reset()
  {
    if (bindings_.created()) bindings_.clear();
  }

private:
  using Entry = common::hash::HashMapTypes<const ParseNode *, ParameterBinding>::AllocType;
  common::hash::ObHashMap<const ParseNode *, ParameterBinding, common::hash::NoPthreadDefendMode,
      SyntaxIdentityHash, std::equal_to<const ParseNode *>, common::hash::SimpleAllocer<Entry>,
      common::hash::NormalPointer, common::ObMalloc, 2> bindings_;
};
}
