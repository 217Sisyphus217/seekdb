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

#include "sql/parser/fts_parse.h"
#include "sql/parser/parse_malloc.h"
#include "rust/sql_parser.h"

namespace {
using namespace oceanbase::sql;

int build_search_node(const peg::SearchNode &source, void *pool, FtsNode *&node)
{
  node = static_cast<FtsNode *>(parse_malloc(sizeof(FtsNode), pool));
  if (node == nullptr) return FTS_ERROR_MEMORY;
  switch (source.kind) {
    case peg::SearchNodeKind::Term: node->type = FTS_NODE_TERM; break;
    case peg::SearchNodeKind::List: node->type = FTS_NODE_LIST; break;
    case peg::SearchNodeKind::Group: node->type = FTS_NODE_SUBEXP_LIST; break;
    case peg::SearchNodeKind::Required: node->type = FTS_NODE_OPER; node->oper = FTS_EXIST; break;
    case peg::SearchNodeKind::Excluded: node->type = FTS_NODE_OPER; node->oper = FTS_IGNORE; break;
    default: return FTS_ERROR_OTHER;
  }
  if (source.kind == peg::SearchNodeKind::Term) {
    node->term.str_ = parse_strndup(source.text.data(), source.text.size(), pool);
    if (node->term.str_ == nullptr) return FTS_ERROR_MEMORY;
    node->term.len_ = source.text.size();
  }
  for (const auto &item : source.children) {
    FtsNode *child = nullptr;
    const int ret = build_search_node(item, pool, child);
    if (ret != FTS_OK) return ret;
    if (node->list.tail == nullptr) node->list.head = child;
    else node->list.tail->next = child;
    node->list.tail = child;
  }
  return FTS_OK;
}
}

void fts_parse_docment(const char *input, int length, void *pool, FtsParserResult *result)
{
  *result = {};
  auto parsed = oceanbase::sql::peg::parse_search_query(
      rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(input), length));
  if (!parsed.error.empty()) {
    result->err_info_.str_ = parse_strndup(parsed.error.data(), parsed.error.size(), pool);
    result->err_info_.len_ = parsed.error.size();
    result->ret_ = result->err_info_.str_ == nullptr ? FTS_ERROR_MEMORY : FTS_ERROR_SYNTAX;
  } else if (!parsed.root.children.empty()) {
    result->ret_ = build_search_node(parsed.root, pool, result->root_);
  }
}
