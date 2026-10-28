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

#include "ob_pl_parser.h"
#include "sql/parser/rust_sql_parser.h"

namespace oceanbase::pl
{
int ObPLParser::parse(const common::ObString &source, sql::ParsedSql &syntax)
{
  auto &result = syntax.metadata();
  result.malloc_pool_ = &allocator_;
  result.sql_mode_ = sql_mode_;
  return sql::parse_rust_sql(source, result, true, &syntax);
}
}  // namespace oceanbase::pl
