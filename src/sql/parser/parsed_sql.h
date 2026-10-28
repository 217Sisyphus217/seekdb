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

class ParsedSql {
public:
  ParsedSql() = default;
  ParsedSql(const ParsedSql &) = delete;
  ParsedSql &operator=(const ParsedSql &) = delete;

  ParseResult &legacy() { return legacy_; }
  const ParseResult &legacy() const { return legacy_; }
  const peg::Node *statement() const { return syntax_ ? &syntax_->root : nullptr; }
  int statement_type(stmt::StmtType &type) const;
  void sql_traits(ObSqlTraits &traits);
  bool is_end_transaction() const;
  void reset() { syntax_.reset(); legacy_ = {}; }

private:
  friend int parse_rust_sql(const common::ObString &, ParseResult &, bool, ParsedSql *);
  ParseResult legacy_{};
  std::optional<peg::ParseTree> syntax_;
};
}
