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

#define USING_LOG_PREFIX SQL_PARSER
#include "ob_fast_parser.h"
#include "rust/sql_parser.h"
#include "share/ob_define.h"
#include "lib/worker.h"
#include "sql/parser/parse_malloc.h"

using namespace oceanbase::sql;
using namespace oceanbase::common;

namespace {
int scan(const ObString &stmt, const FPContext &context, ObIAllocator &allocator,
         ObFastParserResult &result)
{
  peg::ScanOptions options{
      peg::SqlMode{bool(context.sql_mode_ & SMO_ANSI_QUOTES), bool(context.sql_mode_ & SMO_NO_BACKSLASH_ESCAPES),
                   bool(context.sql_mode_ & SMO_PIPES_AS_CONCAT), bool(context.sql_mode_ & SMO_HIGH_NOT_PRECEDENCE)},
      context.parameterize_, context.ignore_hint_, context.is_format_, context.enable_batched_multi_stmt_};
  int ret = OB_SUCCESS;
  if (OB_FAIL(THIS_WORKER.check_status())) return ret;
  auto scanner = peg::begin_scan(rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(stmt.ptr()), stmt.length()), options);
  bool finished = false;
  do {
    finished = scanner->advance();
    if (OB_FAIL(THIS_WORKER.check_status())) return ret;
  } while (!finished);
  auto scanned = peg::finish_scan(std::move(scanner));
  if (!scanned.error.empty()) return OB_ERR_PARSER_SYNTAX;
  auto copy = [&](const char *text, size_t size) -> char * {
    char *owned = static_cast<char *>(allocator.alloc(size + 1));
    if (owned != nullptr) { MEMCPY(owned, text, size); owned[size] = '\0'; }
    return owned;
  };
  char *sql = copy(scanned.sql.data(), scanned.sql.size());
  if (sql == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
  result.pc_key_.name_.assign_ptr(sql, scanned.sql.size());
  result.values_token_pos_ = scanned.insert_values_offset;
  const int64_t count = context.parameterize_ ? scanned.literals.size() : 0;
  result.raw_params_.reset();
  if (count > 0) {
    auto *tokens = static_cast<LiteralToken *>(allocator.alloc(sizeof(LiteralToken) * count));
    auto *params = static_cast<ObPCParam *>(allocator.alloc(sizeof(ObPCParam) * count));
    if (tokens == nullptr || params == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
    if (OB_FAIL(result.raw_params_.reserve(count))) return ret;
    const char *text = nullptr;
    if (!scanned.literal_text.empty()) {
      text = copy(reinterpret_cast<const char *>(scanned.literal_text.data()), scanned.literal_text.size());
      if (text == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
    }
    int64_t i = 0;
    for (const auto &literal : scanned.literals) {
      auto *token = new (tokens + i) LiteralToken{
          literal.kind, literal.value, ObString(),
          ObString(literal.end - literal.start, stmt.ptr() + literal.start),
          literal.concatenated, static_cast<int32_t>(literal.first_part_length)};
      if (token->is_numeric()) {
        token->text = token->source;
      } else if (literal.content_start != literal.content_end) {
        token->text.assign_ptr(text + literal.content_start, literal.content_end - literal.content_start);
      }
      auto *param = new (params + i) ObPCParam();
      param->literal_ = token;
      param->source_offset_ = literal.start;
      param->sql_position_ = literal.sql_offset;
      if (OB_FAIL(result.raw_params_.push_back(param))) return ret;
      ++i;
    }
  }
  {
    auto &placeholders = result.question_mark_ctx_;
    result.reset_question_mark_ctx();
    placeholders.by_ordinal_ = scanned.ordinal_parameters > 0;
    placeholders.by_name_ = !scanned.parameter_names.empty();
    placeholders.count_ = scanned.ordinal_parameters + scanned.parameter_names.size();
    placeholders.capacity_ = scanned.parameter_names.size();
    if (!scanned.parameter_names.empty()) {
      placeholders.name_ = static_cast<char **>(allocator.alloc(sizeof(char *) * placeholders.capacity_));
      if (placeholders.name_ == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
      for (int i = 0; i < placeholders.capacity_; ++i) {
        const auto &name = scanned.parameter_names[i];
        placeholders.name_[i] = copy(name.data(), name.size());
        if (placeholders.name_[i] == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    for (const auto &position : scanned.values) {
      if (OB_FAIL(result.values_tokens_.push_back(ObValuesTokenPos(position.sql_offset, position.literal_index)))) break;
    }
  }
  return ret;
}
}

int ObFastParser::parse(const common::ObString &stmt,
                        const FPContext &context,
                        common::ObIAllocator &allocator,
                        ObFastParserResult &result)
{
  return scan(stmt, context, allocator, result);
}

namespace {
enum ROW_STATE { START_STATE, LEFT_PAR_STATE, PARS_MATCH, ON_DUPLICATE_KEY, UNEXPECTED_STATE };
enum TRIM_STATE { START_TRIM_STATE, WITH_SPLII_CH, WITH_OTHER_CH, WITH_SPACE_CH, TRIM_FAILED };
bool space_at(const ObRawSql &sql, int64_t pos, int64_t &width)
{
  const bool is_space = pos < sql.raw_sql_len_ && SPACE_FLAGS[static_cast<uint8_t>(sql.raw_sql_[pos])];
  width = is_space ? 1 : 0;
  return is_space;
}
bool is_comma(char ch) { return ch == ','; }
bool is_left_parenthesis(char ch) { return ch == '('; }
bool is_right_parenthesis(char ch) { return ch == ')'; }
int copy_trimed_data_buff(char *new_sql_buf,
                                            int64_t buf_len,
                                            int64_t &pos,
                                            const int64_t start_pos,
                                            const int64_t end_pos,
                                            ObRawSql &raw_sql);
int do_trim_for_insert(char *new_sql_buf,
                                         int64_t buff_len,
                                         const ObString &no_trim_sql,
                                         ObString &after_trim_sql,
                                         bool &trimed_succ);
bool skip_space(ObRawSql &raw_sql);
bool is_split_character(ObRawSql &raw_sql);
int check_is_on_duplicate_key(ObRawSql &raw_sql, bool &is_on_duplicate_key);
int get_one_insert_row_str(ObRawSql &raw_sql,
                                             ObString &str,
                                             bool &is_valid,
                                             int64_t &ins_params_count,
                                             bool &is_insert_up,
                                             int64_t &on_duplicate_params,
                                             int64_t &end_pos);

int copy_trimed_data_buff(char *new_sql_buf,
                                            int64_t buf_len,
                                            int64_t &pos,
                                            const int64_t start_pos,
                                            const int64_t end_pos,
                                            ObRawSql &raw_sql)
{
  int ret = OB_SUCCESS;
  LOG_DEBUG("print copy_trimed_data_buff", K(start_pos), K(end_pos), K(raw_sql.to_string()));
  if (start_pos < end_pos) {
    if (OB_FAIL(databuff_memcpy(new_sql_buf, buf_len, pos, end_pos - start_pos, raw_sql.ptr(start_pos)))) {
    }
  }
  return ret;
}

int do_trim_for_insert(char *new_sql_buf,
                                         int64_t buff_len,
                                         const ObString &no_trim_sql,
                                         ObString &after_trim_sql,
                                         bool &trimed_succ)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  trimed_succ = false;
  int64_t space_len = 0;
  after_trim_sql.reset();
  ObRawSql raw_sql;
  raw_sql.init(no_trim_sql.ptr(), no_trim_sql.length());
  int64_t start_pos = raw_sql.cur_pos_;
  int64_t end_pos = raw_sql.cur_pos_;
  TRIM_STATE trim_state = START_TRIM_STATE;
  while(trim_state != TRIM_FAILED && OB_SUCC(ret) && !raw_sql.is_search_end()) {
    char ch = raw_sql.char_at(raw_sql.cur_pos_);
    switch (trim_state) {
      case START_TRIM_STATE:
        if (space_at(raw_sql, raw_sql.cur_pos_, space_len)) {
          skip_space(raw_sql);
          start_pos = raw_sql.cur_pos_;
        } else if (is_split_character(raw_sql)) {
          trim_state = WITH_SPLII_CH;
        } else {
          raw_sql.scan();
          trim_state = WITH_OTHER_CH;
        }
        break;
      case WITH_SPLII_CH:
        if (space_at(raw_sql, raw_sql.cur_pos_, space_len)) {
          end_pos = raw_sql.cur_pos_;
          if (OB_FAIL(copy_trimed_data_buff(new_sql_buf, buff_len, pos, start_pos, end_pos, raw_sql))) {
          } else {
            skip_space(raw_sql);
            start_pos = raw_sql.cur_pos_;
            end_pos = start_pos;
          }
        } else if (is_split_character(raw_sql)) {
          // do nothing
        } else {
          raw_sql.scan();
          trim_state = WITH_OTHER_CH;
        }
        break;
      case WITH_OTHER_CH:
        if (space_at(raw_sql, raw_sql.cur_pos_, space_len)) {
          end_pos = raw_sql.cur_pos_;
          trim_state = WITH_SPACE_CH;
          if (OB_FAIL(copy_trimed_data_buff(new_sql_buf, buff_len, pos, start_pos, end_pos, raw_sql))) {
          } else {
            skip_space(raw_sql);
            start_pos = raw_sql.cur_pos_;
            end_pos = start_pos;
          }
        } else if (is_split_character(raw_sql)) {
          trim_state = WITH_SPLII_CH;
        } else {
          raw_sql.scan();
        }
        break;
      case WITH_SPACE_CH:
        if (space_at(raw_sql, raw_sql.cur_pos_, space_len)) {
          end_pos = raw_sql.cur_pos_;
          if (OB_FAIL(copy_trimed_data_buff(new_sql_buf, buff_len, pos, start_pos, end_pos, raw_sql))) {
          } else {
            skip_space(raw_sql);
            start_pos = raw_sql.cur_pos_;
            end_pos = start_pos;
          }
        } else if (is_split_character(raw_sql)) {
          trim_state = WITH_SPLII_CH;
        } else {
          trim_state = TRIM_FAILED;
        }
        break;
      case TRIM_FAILED:
        // do nothing
        break;
      }

    if (raw_sql.is_search_end()) {
      end_pos = raw_sql.cur_pos_;
      if (OB_FAIL(copy_trimed_data_buff(new_sql_buf, buff_len, pos, start_pos, end_pos, raw_sql))) {
      }
    }
  } // end while


  if (OB_SUCC(ret) && trim_state != TRIM_FAILED) {
    after_trim_sql.assign_ptr(new_sql_buf, pos);
    trimed_succ = true;
  }

  return ret;
}

bool skip_space(ObRawSql &raw_sql)
{
  bool b_ret = false;
  int64_t space_len = 0;
  while (!raw_sql.search_end_ && space_at(raw_sql, raw_sql.cur_pos_, space_len)) {
    raw_sql.scan(space_len);
    b_ret = true;
  }
  return b_ret;
}

bool is_split_character(ObRawSql &raw_sql)
{
  bool bret = false;
  char ch = raw_sql.char_at(raw_sql.cur_pos_);
  if (is_comma(ch) || is_left_parenthesis(ch) || is_right_parenthesis(ch)) {
    raw_sql.scan();
    bret = true;
  }
  return bret;
}

int check_is_on_duplicate_key(ObRawSql &raw_sql, bool &is_on_duplicate_key)
{
  int ret = OB_SUCCESS;
  is_on_duplicate_key = false;
  if (0 == raw_sql.strncasecmp("duplicate", 9)) {
    raw_sql.scan(9);
    bool has_space = skip_space(raw_sql);
    if (has_space && 0 == raw_sql.strncasecmp("key", 3)) {
      raw_sql.scan(3);
      has_space = skip_space(raw_sql);
      if (has_space && 0 == raw_sql.strncasecmp("update", 6)) {
        raw_sql.scan(6);
        is_on_duplicate_key = true;
      }
    }
  }
  return ret;
}

int get_one_insert_row_str(ObRawSql &raw_sql,
                                             ObString &str,
                                             bool &is_valid,
                                             int64_t &ins_params_count,
                                             bool &is_insert_up,
                                             int64_t &on_duplicate_params,
                                             int64_t &end_pos)
{
  int ret = OB_SUCCESS;
  int left_count = 0;
  int64_t start = 0;
  int64_t end = 0;
  bool need_break = false;
  int64_t curr_pos = 0;
  is_valid = false;
  is_insert_up = false;
  on_duplicate_params = 0;
  ins_params_count = 0;
  end_pos = 0;
  ROW_STATE row_state = START_STATE;

  while (!need_break && !raw_sql.is_search_end()) {
    skip_space(raw_sql);
    char ch = raw_sql.char_at(raw_sql.cur_pos_);
    curr_pos = raw_sql.cur_pos_;
    raw_sql.scan();
    switch (row_state) {
      case START_STATE:
        if ('(' == ch) {
          start = curr_pos;
          row_state = LEFT_PAR_STATE;
          left_count++;
        } else {
          row_state = UNEXPECTED_STATE;
        }
        break;
      case LEFT_PAR_STATE:
        if (')' == ch) {
          left_count--;
          if (0 == left_count) {
            row_state = PARS_MATCH;
            end = curr_pos;
          }
        } else if ('(' == ch) {
          left_count++;
        } else if ('?' == ch) {
          ins_params_count++;
        }
        break;
      case PARS_MATCH:
        if (',' == ch) {
          need_break = true;
        } else if (';' == ch) {
          raw_sql.search_end_ = true;
          need_break = true;
        } else if ('o' == ch || 'O' == ch) {
          bool is_duplicate = false;
          if (0 == raw_sql.strncasecmp("n", 1)) {
            raw_sql.scan();
            bool has_space = skip_space(raw_sql);
            bool is_duplicate = false;
            if (has_space) {
              check_is_on_duplicate_key(raw_sql, is_duplicate);
              if (is_duplicate) {
                row_state = ON_DUPLICATE_KEY;
              } else {
                row_state = UNEXPECTED_STATE;
              }
            }
          }
        } else {
          row_state = UNEXPECTED_STATE;
        }
        break;
      case ON_DUPLICATE_KEY:
        if ('?' == ch) {
          on_duplicate_params++;
        }
        break;
      case UNEXPECTED_STATE:
        need_break = true;
        break;
      default:
        break;
    }
  }

  if (PARS_MATCH == row_state || ON_DUPLICATE_KEY == row_state) {
    if (ON_DUPLICATE_KEY == row_state) {
      is_insert_up = true;
    }
    if (start > 0 && end > 0) {
      str.assign_ptr(raw_sql.ptr(start), (end - start) + 1);
      end_pos = end;
      is_valid = true;
    }
  }
  return ret;
}
}

int ObFastParser::compress_insert(common::ObIAllocator &allocator,
                                        int64_t values_token_pos,
                                        const ObString &old_no_param_sql,
                                        ObString &new_truncated_sql,
                                        bool &can_batch_opt,
                                        int64_t &params_count,
                                        int64_t &upd_params_count,
                                        int64_t &lenth_delta,
                                        int64_t &row_count)
{
  int ret = OB_SUCCESS;
  can_batch_opt = false;
  params_count = 0;
  row_count = 0;
  upd_params_count = 0;
  lenth_delta = 0;
  bool is_valid = false;
  bool is_insert_up = false;
  int64_t first_end_pos = 0;
  if (values_token_pos != 0) {
    ObRawSql raw_sql;
    raw_sql.init(old_no_param_sql.ptr(), old_no_param_sql.length());
    raw_sql.cur_pos_ = values_token_pos - 1;
    if (0 == raw_sql.strncasecmp(raw_sql.cur_pos_, "values", 6)) {
      bool is_first = true;
      is_valid = true;
      ObString first_str;
      ObString first_trimed_str;
      ObString cur_str;
      ObString cur_trimed_str;
      int64_t end_pos = 0;
      char *first_str_buf = nullptr;
      char *other_str_buf = nullptr;
      int64_t first_str_buf_len = 0;

      // scan the length of 'values'
      raw_sql.scan(6);
      while (OB_SUCC(ret) && is_valid && !raw_sql.is_search_end()) {
        cur_str.reset();
        if (is_insert_up) {
          is_valid = false;
        } else if (OB_FAIL(get_one_insert_row_str(raw_sql,
                                                  cur_str,
                                                  is_valid,
                                                  params_count,
                                                  is_insert_up,
                                                  upd_params_count,
                                                  end_pos))) {
        } else if (!is_valid) {
          LOG_WARN("get insert row failed", K(raw_sql.to_string()));
        } else if (is_first) {
          is_first = false;
          first_str.assign_ptr(cur_str.ptr(), cur_str.length());
          new_truncated_sql.assign_ptr(old_no_param_sql.ptr(), end_pos + 1);
          first_end_pos = end_pos;
          row_count++;
        } else if (first_str != cur_str) {
          is_valid = false;
          row_count++;
          bool trimed_succ = false;
          if (first_str_buf == nullptr) {
            first_str_buf_len = first_str.length() + 1; // copy function requires that there must be one position at the end filled with '\0'
            if (OB_ISNULL(first_str_buf = static_cast<char*>(allocator.alloc(first_str_buf_len)))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_ISNULL(other_str_buf = static_cast<char*>(allocator.alloc(first_str_buf_len)))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(do_trim_for_insert(first_str_buf,
                                                  first_str_buf_len,
                                                  first_str,
                                                  first_trimed_str,
                                                  trimed_succ))) {
            } else if (!trimed_succ) {
              // trim failed
            } else if (OB_FAIL(do_trim_for_insert(other_str_buf,
                                                  first_str_buf_len,
                                                  cur_str,
                                                  cur_trimed_str,
                                                  trimed_succ))) {
            } else if (!trimed_succ) {
              // trim failed
            } else if (first_trimed_str == cur_trimed_str) {
              is_valid = true;
            }
          } else if (FALSE_IT(cur_trimed_str.reset())) {
            // cur_trimed_str will be reused，need do reset, other_str_buf also will be reuse
          } else if (OB_ISNULL(other_str_buf)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(do_trim_for_insert(other_str_buf,
                                                first_str_buf_len,
                                                cur_str,
                                                cur_trimed_str,
                                                trimed_succ))) {
          } else if (!trimed_succ) {
            // trim failed
          } else if (first_trimed_str == cur_trimed_str) {
            is_valid = true;
          }
        } else {
          row_count++;
        }

        if (OB_FAIL(ret)) {
          // do nothing
        } else if (is_valid && is_insert_up) {
          char *new_sql_buf = NULL;
          int64_t pos = 0;
          int64_t on_duplicate_pos = end_pos + 1;
          int64_t on_duplicate_length = old_no_param_sql.length() - on_duplicate_pos;
          int64_t insert_length = new_truncated_sql.length();
          int64_t final_length = insert_length + on_duplicate_length + 1;
          lenth_delta = end_pos - first_end_pos;
          if (OB_ISNULL(new_sql_buf = static_cast<char*>(allocator.alloc(final_length)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(databuff_memcpy(
              new_sql_buf, final_length, pos, insert_length, old_no_param_sql.ptr()))) {
          } else if (OB_FAIL(databuff_memcpy(
              new_sql_buf, final_length, pos, (on_duplicate_length), (old_no_param_sql.ptr() + on_duplicate_pos)))) {
          } else {
            new_truncated_sql.reset();
            new_truncated_sql.assign_ptr(new_sql_buf, pos);
          }
        }
      } // end while
      can_batch_opt = is_valid;
    }
  }
  return ret;
}
