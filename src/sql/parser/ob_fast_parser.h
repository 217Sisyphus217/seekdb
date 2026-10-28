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

#ifndef OCEANBASE_SQL_PARSER_FAST_PARSER_
#define OCEANBASE_SQL_PARSER_FAST_PARSER_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/charset/ob_charset.h"
#include "sql/parser/ob_parser_utils.h"
#include "sql/parser/ob_char_type.h"
#include "sql/parser/parse_malloc.h"
#include "sql/plan_cache/ob_plan_cache_struct.h"

namespace oceanbase
{
namespace sql
{
struct FPContext
{
public:
	bool enable_batched_multi_stmt_;
	ObCharsets4Parser charsets4parser_;
	ObSQLMode sql_mode_;
  bool is_format_;
  bool parameterize_ = true;
  bool ignore_hint_ = false;

	FPContext()
		: enable_batched_multi_stmt_(false),
			sql_mode_(0),
      is_format_(false)
	{}
	FPContext(ObCharsets4Parser charsets4parser)
		: enable_batched_multi_stmt_(false),
			charsets4parser_(charsets4parser),
			sql_mode_(0),
      is_format_(false)
	{}
};

struct ObFastParser final
{
public:
  static int parse(const common::ObString &stmt,
                   const FPContext &context,
                   common::ObIAllocator &allocator,
                   ObFastParserResult &result);
  static int compress_insert(common::ObIAllocator &allocator,
                                        int64_t values_token_pos,
                                        const ObString &old_no_param_sql,
                                        ObString &new_truncated_sql,
                                        bool &can_batch_opt,
                                        int64_t &params_count,
                                        int64_t &upd_params_count,
                                        int64_t &lenth_delta,
                                        int64_t &row_count);

};

static const char INVALID_CHAR = -1;
struct ObRawSql {
	explicit ObRawSql() :
		raw_sql_(nullptr), raw_sql_len_(0),
		cur_pos_(0), search_end_(false) {}

	inline void init(const char *raw_sql, const int64_t len)
	{
		cur_pos_ = 0;
		raw_sql_ = raw_sql;
		raw_sql_len_ = len;
	}
	inline bool is_search_end()
	{
		return search_end_ || cur_pos_ > raw_sql_len_ - 1;
	}
	inline bool is_search_end(const int64_t pos)
	{
		return search_end_ || pos > raw_sql_len_ - 1;
	}
	inline char peek()
	{
		if (cur_pos_ >= raw_sql_len_ - 1) {
			return INVALID_CHAR;
		}
		return raw_sql_[cur_pos_ + 1];
	}
	inline char scan(const int64_t offset)
	{
		if (cur_pos_ + offset >= raw_sql_len_) {
			search_end_ = true;
			cur_pos_ = raw_sql_len_;
			return INVALID_CHAR;
		}
		cur_pos_ += offset;
		return raw_sql_[cur_pos_];
	}
	inline char scan() { return scan(1); }
	inline char reverse_scan()
	{
		if (cur_pos_ <= 0 || cur_pos_ >= raw_sql_len_ + 1) {
			search_end_ = true;
			return INVALID_CHAR;
		}
		return raw_sql_[--cur_pos_];
	}
	inline char char_at(int64_t idx)
	{
		if (idx < 0 || idx >= raw_sql_len_) {
			return INVALID_CHAR;
		}
		return raw_sql_[idx];
	}
	inline const char *ptr(const int64_t pos)
	{
		if (pos < 0 || pos >= raw_sql_len_) {
			return nullptr;
		}
		return &(raw_sql_[pos]);
	}
	inline int64_t strncasecmp(int64_t pos, const char *str, const int64_t size)
	{
		// It is not necessary to check if str is nullptr
		char ch = char_at(pos);
		for (int64_t i = 0; i < size; i++) {
			if (ch >= 'A' && ch <= 'Z') {
				ch += 32;
			}
			if (ch != str[i]) {
				return -1;
			}
			ch = char_at(++pos);
		}
		return 0;
	}
	inline int64_t strncasecmp(const char *str, const int64_t size)
	{
		return strncasecmp(cur_pos_, str, size);
	}
	// Access a character (no bounds check)
	// char operator[] (const int64_t idx) const { return raw_sql_[idx]; }
	// For debug
	common::ObString to_string() const
	{
		if (OB_UNLIKELY(nullptr == raw_sql_ || 0 == raw_sql_len_)) {
			return common::ObString(0, nullptr);
		}
		return common::ObString(raw_sql_len_, raw_sql_);
	}

	const char *raw_sql_;
	int64_t raw_sql_len_;
	int64_t cur_pos_;
	bool search_end_;
};

} // namespace sql
} // namespace oceanbase

#endif
