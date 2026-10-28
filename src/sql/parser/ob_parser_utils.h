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

#ifndef _OB_PARSER_UTILS_H
#define _OB_PARSER_UTILS_H 1
#include "lib/utility/ob_print_utils.h"
#include "lib/charset/ob_charset.h"
#include "lib/ob_name_def.h"
#include "lib/utility/ob_smart_call.h"
#include "lib/utility/ob_hang_fatal_error.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
struct ObCharsets4Parser
{
  explicit ObCharsets4Parser() :
      string_collation_(common::CS_TYPE_UTF8MB4_GENERAL_CI),
      nls_collation_(common::CS_TYPE_INVALID)
    {}
  common::ObCollationType string_collation_; //collation type for the string to parse
  common::ObCollationType nls_collation_; // database NLS collation for validating identifiers
};

} // end namespace sql
} // end namespace oceanbase


#endif /* _OB_PARSER_UTILS_H */
