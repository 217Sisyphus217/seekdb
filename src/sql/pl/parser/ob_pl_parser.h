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

#ifndef OCEANBASE_SRC_PL_PARSER_OB_PL_PARSER_H_
#define OCEANBASE_SRC_PL_PARSER_OB_PL_PARSER_H_

#include "sql/parser/parsed_sql.h"
#include "share/ob_define.h"
#include "share/schema/ob_trigger_info.h"
#include "sql/parser/ob_parser_utils.h"
#include "lib/ob_date_unit_type.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
class ObString;
}  // namespace common
namespace sql
{
class ObSQLSessionInfo;
}

namespace pl
{
class ObPLParser
{
public:
  ObPLParser(common::ObIAllocator &allocator, ObSQLMode sql_mode = 0)
    : allocator_(allocator),
      sql_mode_(sql_mode)
  {}
  int parse(const common::ObString &source, sql::ParsedSql &syntax);
private:
  common::ObIAllocator &allocator_;
  ObSQLMode sql_mode_;
};
}  // namespace pl
}  // namespace oceanbase
#endif /* OCEANBASE_SRC_PL_PARSER_OB_PL_PARSER_H_ */
