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

#pragma once

#include <optional>
#include "rust/sql_parser.h"
#include "sql/parser/parse_node.h"
#include "share/statement/ob_stmt_type.h"

namespace oceanbase::sql {
struct ObSqlTraits;
int statement_type(const peg::Node &statement, stmt::StmtType &type);

class ParsedSql {
public:
  ParsedSql() = default;
  ParsedSql(ParsedSql &&) = default;
  ParsedSql &operator=(ParsedSql &&) = default;
  ParsedSql(const ParsedSql &) = delete;
  ParsedSql &operator=(const ParsedSql &) = delete;

  ParseResult &metadata() { return metadata_; }
  const ParseResult &metadata() const { return metadata_; }
  const peg::Node *statement() const { return syntax_ ? &syntax_->root : nullptr; }
  common::ObString source() const
  {
    return syntax_ && !syntax_->expanded_sql.empty()
        ? common::ObString(syntax_->expanded_sql.size(), syntax_->expanded_sql.data())
        : original_source_;
  }
  common::ObString original_source() const { return original_source_; }
  common::ObString source(const peg::Node &node) const
  {
    return common::ObString(node.end - node.start, source().ptr() + node.start);
  }
  int preserve_source(common::ObIAllocator &allocator);
  int statement_type(stmt::StmtType &type) const;
  void sql_traits(ObSqlTraits &traits);
  bool is_end_transaction() const;
  void reset() { syntax_.reset(); original_source_.reset(); metadata_ = {}; }

private:
  friend int parse_rust_sql(const common::ObString &, ParseResult &, bool, ParsedSql *);
  ParseResult metadata_{};
  common::ObString original_source_;
  std::optional<peg::ParseTree> syntax_;
};
}
