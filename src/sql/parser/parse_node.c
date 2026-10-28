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

#include "sql/parser/parse_node.h"
#include <string.h>
#include "lib/utility/alloc_assist.h"
#include "sql/parser/parse_node_hash.h"
#include "sql/parser/parse_define.h"
#include "sql/parser/parse_malloc.h"
#include <stdio.h>
#include <ctype.h>
#include "sql/executor/ob_memory_tracker_wrapper.h"
extern const char *get_type_name(int type);

int get_deep_copy_size(const ParseNode *node, int64_t *size)
{
  int ret = OB_PARSER_SUCCESS;
  if (OB_UNLIKELY(NULL == node)) {
    ret = OB_PARSER_ERR_UNEXPECTED;
    (void)fprintf(stderr, "ERROR node is null\n");
  } else if (OB_UNLIKELY(NULL == size)) {
    ret = OB_PARSER_ERR_UNEXPECTED;
    (void)fprintf(stderr, "ERROR size is null\n");
  } else {
    *size += sizeof(ParseNode);
    if (node->str_len_ > 0 && node->str_value_ != NULL) {
      *size += node->str_len_ + 1;
    }
    if (node->text_len_ > 0 && node->raw_text_ != NULL) {
      *size += node->text_len_ + 1;
    }
    if (node->num_child_ > 0) {
      if (OB_UNLIKELY(NULL == node->children_)) {
        ret = OB_PARSER_ERR_UNEXPECTED;
        (void)fprintf(stderr, "ERROR children is null\n");
      } else {
        *size += (sizeof(ParseNode*) * node->num_child_);
      }
      for (int64_t i = 0; OB_PARSER_SUCCESS == ret && i < node->num_child_; ++i) {
        ParseNode *child_node = node->children_[i];
        if (OB_UNLIKELY(NULL == child_node)) {
          ret = OB_PARSER_ERR_UNEXPECTED;
          (void)fprintf(stderr, "ERROR child node is null\n");
        } else if (OB_PARSER_SUCCESS != (ret = get_deep_copy_size(child_node, size))) {
          (void)fprintf(stderr, "ERROR failed to get deep copy size\n");
        }
      }
    }
  }
  return ret;
}

int deep_copy_parse_node_base(void *malloc_pool, const ParseNode *src_node, ParseNode *dst_node)
{
  int ret = OB_PARSER_SUCCESS;
  if (OB_UNLIKELY(NULL == src_node) || OB_UNLIKELY(NULL == dst_node)) {
    ret = OB_PARSER_ERR_UNEXPECTED;
    (void)fprintf(stderr, "ERROR node is null\n");
  } else if (OB_UNLIKELY(NULL == malloc_pool)) {
    ret = OB_PARSER_ERR_UNEXPECTED;
    (void)fprintf(stderr, "ERROR malloc pool is null\n");
  } else {
    dst_node->type_ = src_node->type_;
    dst_node->num_child_ = src_node->num_child_;
    dst_node->param_num_ = src_node->param_num_;
    dst_node->flag_ = src_node->flag_;
    dst_node->value_ = src_node->value_;
    dst_node->pos_ = src_node->pos_;
    dst_node->stmt_loc_ = src_node->stmt_loc_;
    dst_node->raw_sql_offset_ = src_node->raw_sql_offset_;
    if (src_node->str_len_ > 0 && src_node->str_value_ != NULL) {
      char *buf = NULL;
      if (OB_UNLIKELY(NULL == (buf = (char *)parser_alloc(malloc_pool, src_node->str_len_ + 1)))) {
        ret = OB_PARSER_ERR_NO_MEMORY;
        (void)fprintf(stderr, "ERROR failed to allocate memory\n");
      } else {
        dst_node->str_len_ = src_node->str_len_;
        MEMCPY(buf, src_node->str_value_, dst_node->str_len_);
        buf[dst_node->str_len_] = '\0';
        dst_node->str_value_ = buf;
      }
    }
    if (OB_PARSER_SUCCESS == ret && src_node->text_len_ > 0 && src_node->raw_text_ != NULL) {
      char *buf = NULL;
      if (OB_UNLIKELY(NULL == (buf = (char *)parser_alloc(malloc_pool, src_node->text_len_ + 1)))) {
        ret = OB_PARSER_ERR_NO_MEMORY;
        (void)fprintf(stderr, "ERROR failed to allocate memory\n");
      } else {
        dst_node->text_len_ = src_node->text_len_;
        dst_node->pos_ = src_node->pos_;
        MEMCPY(buf, src_node->raw_text_, dst_node->text_len_);
        buf[dst_node->text_len_] = '\0';
        dst_node->raw_text_ = buf;
      }
    }
  }
  return ret;
}

int deep_copy_parse_node(void *malloc_pool, const ParseNode *src_node, ParseNode *dst_node)
{
  int ret = OB_PARSER_SUCCESS;
  if (OB_UNLIKELY(OB_PARSER_SUCCESS != (ret = deep_copy_parse_node_base(malloc_pool, src_node, dst_node)))) {
    (void)fprintf(stderr, "ERROR failed to copy parse node base\n");
  } else if (src_node->num_child_ > 0) {
    if (OB_UNLIKELY(NULL == src_node->children_)) {
      ret = OB_PARSER_ERR_UNEXPECTED;
      (void)fprintf(stderr, "ERROR children is null\n");
    } else if (OB_UNLIKELY(NULL == (dst_node->children_ = (ParseNode **)parser_alloc(
                                    malloc_pool, sizeof(ParseNode*) * src_node->num_child_)))) {
      ret = OB_PARSER_ERR_NO_MEMORY;
      (void)fprintf(stderr, "ERROR failed to allocate memory\n");
    }
    for (int64_t i = 0; OB_PARSER_SUCCESS == ret && i < src_node->num_child_; ++i) {
      ParseNode *tmp_node = NULL;
      ParseNode *child_node = src_node->children_[i];
      if (OB_UNLIKELY(NULL == child_node)) {
        ret = OB_PARSER_ERR_UNEXPECTED;
        (void)fprintf(stderr, "ERROR child node is null\n");
      } else if (OB_UNLIKELY(NULL == (tmp_node =
                (ParseNode *)parser_alloc(malloc_pool, sizeof(ParseNode))))) {
        ret = OB_PARSER_ERR_NO_MEMORY;
        (void)fprintf(stderr, "ERROR failed to allocate memory\n");
      } else if (OB_PARSER_SUCCESS != (ret = deep_copy_parse_node(malloc_pool, child_node, tmp_node))) {
        (void)fprintf(stderr, "ERROR failed to deep copy parse node\n");
      } else {
        dst_node->children_[i] = tmp_node;
      }
    }
  }
  return ret;
}

int OB_WEAK_SYMBOL check_mem_status() { return OB_PARSER_SUCCESS; }
int OB_WEAK_SYMBOL try_check_mem_status(int64_t check_try_times) { return OB_PARSER_SUCCESS; }

ParseNode *new_node(void *malloc_pool, ObItemType type, int num)
{
  int ret = OB_PARSER_SUCCESS;
  const int64_t check_try_times = 1024;
  ParseNode *node = NULL;
  // the mem alloced by parse_malloc has been memset;
  if (OB_UNLIKELY((OB_PARSER_SUCCESS != (ret = try_check_mem_status(check_try_times))))) {
    (void)printf("Exceeded memory usage limit\n");
  } else {
    node = (ParseNode *)parse_malloc(sizeof(ParseNode), malloc_pool);
    if (OB_UNLIKELY(NULL == node)) {
      (void)printf("malloc memory failed\n");
    } else {
      node->type_ = type;
      node->num_child_ = num;
      node->value_ = INT64_MAX;
      node->pl_str_off_ = -1;
      if (num > 0) {
        int64_t alloc_size = sizeof(ParseNode *) * num ;
        node->children_ = (ParseNode **)parse_malloc(alloc_size, malloc_pool);
        if (OB_UNLIKELY(NULL == node->children_)) {
          parse_free(node);
          node = NULL;
        }
      } else {
        node->children_ = NULL;
      }
    }
  }
  return node;
}

ParseNode *new_terminal_node(void *malloc_pool, ObItemType type)
{
  int children_num = 0;
  return new_node(malloc_pool, type, children_num);
}

ParseNode *new_non_terminal_node(void *malloc_pool, ObItemType node_tag, int num, ...)
{
  ParseNode *ret_node = NULL;
  if (OB_UNLIKELY(num <= 0)) {
    (void)fprintf(stderr, "ERROR invalid num:%d\n", num);
  } else {
    int32_t i = 0;
    va_list va;
    ret_node = new_node(malloc_pool, node_tag, num);
    if (OB_LIKELY(NULL != ret_node)) {
      va_start(va, num);
      for (; i < num; ++i) {
        ret_node->children_[i] = va_arg(va, ParseNode *);
      }
      va_end(va);
    }
  }
  return ret_node;
}

static char char_int(char c)
{
  return (c >= '0' && c <= '9' ? c-'0': (c >= 'A' && c <= 'Z' ? c - 'A' + 10 : c - 'a' + 10));
}

int64_t ob_parse_binary_len(int64_t len)
{
  return (len + 1) / 2;
}

extern void ob_parse_binary_simd(const char **src, const char *end, char **dest);
void ob_parse_binary(const char *src, int64_t len, char *dest)
{
  if (OB_UNLIKELY(NULL == src || len <= 0 || NULL == dest)) {
    //do nothing
  } else {
    bool is_odd = false;
    if (len > 0 && len % 2 != 0)
    {
      *dest = char_int(src[0]);
      ++src;
      ++dest;
      is_odd = true;
    }
    if (len == 1) {
      //do nothing.
    } else {
      //for odd number, we have copy the first char,  so we should minus 2;
      const char *end = src + len - (is_odd ? 2 : 1);
#if defined(__GNUC__) && defined(__x86_64__) 
      ob_parse_binary_simd(&src, end, &dest);
#endif
      for (; src <= end; src += 2)
      {
        *dest = (char)(16*char_int(src[0]) + char_int(src[1]));
        ++dest;
      }
    }
  }
}

int64_t ob_parse_bit_string_len(int64_t len)
{
  return (len + 7) / 8;
}

void ob_parse_bit_string(const char* src, int64_t len, char* dest)
{
  if (OB_UNLIKELY(NULL == src || len <= 0 || NULL == dest)) {
    //do nothing
  } else {
    const char* end = src + len -1;
    char* dest_end = dest + ob_parse_bit_string_len(len) - 1;
    if (len > 0)
    {
      unsigned char c = 0;
      unsigned int one_bit = 1;
      for ( ; end >= src; --end)
      {
        if (256 == one_bit)         /* one byte ready */
        {
          *dest_end = c;
          --dest_end;
          c = 0;
          one_bit = 1;
        }
        if ('1' == *end)
        {
          c |= one_bit;
        }
        one_bit <<= 1;
      } /* end for */
      *dest_end = c;              /* the first byte */
    }
  }
}

char *str_tolower(char *buff, int64_t len)
{
  if (OB_LIKELY(NULL != buff)) {
    char *ptr = buff;
	char *end = buff + len;
    unsigned char ch = *ptr;
    while (ptr != end) {
      ch = *ptr;
      if (ch >= 'A' && ch <= 'Z') {
        ch += 'a' - 'A';
      } else if (ch >= 0x80 && isupper(ch)) {
        ch = tolower(ch);
      }
      *ptr = ch;
      ptr++;
    }
  }
  return buff;
}

char *str_toupper(char *buff, int64_t len)
{
  if (OB_LIKELY(NULL != buff)) {
    char *ptr = buff;
	  char *end = buff + len;
    unsigned char ch = *ptr;
    while (ptr != end) {
      ch = *ptr;
      if (ch >= 'a' && ch <= 'z') {
        ch -= 'a' - 'A';
      } else if (ch >= 0x80 && islower(ch)) {
        ch = toupper(ch);
      }
      *ptr = ch;
      ptr++;
    }
  }
  return buff;
}

// calculate hash value of syntax tree recursively
// every member of ParseNode is calculated using murmurhash
uint64_t parsenode_hash(const ParseNode *node, int *ret)
{
  uint64_t hash_val = 0;
  if (check_stack_overflow_c()) {
    (void)fprintf(stderr, "ERROR stack overflow in recursive function\n");
    if (ret) {
      *ret = OB_PARSER_ERR_SIZE_OVERFLOW;
    }
  } else if (OB_LIKELY(NULL != node)) {
    hash_val = murmurhash(&node->type_, sizeof(node->type_), hash_val);
    hash_val = murmurhash(&node->value_, sizeof(node->value_), hash_val);
    hash_val = murmurhash(&node->str_len_, sizeof(node->str_len_), hash_val);
    if (NULL != node->str_value_) {
      hash_val = murmurhash(node->str_value_, (int32_t)node->str_len_, hash_val);
    }

    uint64_t child_hash_val = 0;
    int32_t i = 0;
    if (node->num_child_ > 0) {
      if (OB_UNLIKELY(NULL == node->children_)) {
        (void)fprintf(stderr, "ERROR children of node is NULL\n");
      } else {
        for (; i < node->num_child_; ++i) {
          if (NULL != node->children_[i]) {
            child_hash_val = parsenode_hash(node->children_[i], ret);
            if (ret && OB_PARSER_ERR_SIZE_OVERFLOW == *ret) {
              break;
            }
            hash_val = murmurhash(&child_hash_val, sizeof(child_hash_val), hash_val);
          }
        }
      }
    }
  }
  return hash_val;
}

// compare syntax tree recursively
// every member of ParseNode is compared
bool parsenode_equal(const ParseNode *lnode, const ParseNode *rnode, int *ret)
{
  bool result = true;
  if (check_stack_overflow_c()) {
    (void)fprintf(stderr, "ERROR stack overflow in recursive function\n");
    if (ret) {
      *ret = OB_PARSER_ERR_SIZE_OVERFLOW;
    }
  } else if (NULL == lnode && NULL == rnode) {
    result = true;
  } else if ((NULL == lnode && NULL != rnode)
             || (NULL != lnode && NULL == rnode)) {
    result = false;
  } else {
    if (lnode->type_ != rnode->type_
        || lnode->value_ != rnode->value_
        || lnode->str_len_ != rnode->str_len_
        || lnode->num_child_ != rnode->num_child_) {
      result = false;
    } else {
      if (NULL == lnode->str_value_ && NULL == rnode->str_value_) {
        result = true;
      } else if ((NULL == lnode->str_value_ && NULL != rnode->str_value_)
                 || (NULL != lnode->str_value_ && NULL == rnode->str_value_ )) {
        result = false;
      } else if (lnode->str_len_ != rnode->str_len_) {
        result = false;
      } else {
        // T_VARCHAR type: value_ is length, str_value_ is ptr
        // @ref ob_raw_expr.cpp
        if (0 != strncmp(lnode->str_value_, rnode->str_value_, lnode->str_len_)){
          result = false;
        }
      }
      if (result) {
        if (lnode->num_child_ > 0) {
          if (NULL == lnode->children_ || NULL == rnode->children_) {
            result = false;
          } else {
            int32_t i = 0;
            for (; result && i < lnode->num_child_; ++i) {
              result = parsenode_equal(lnode->children_[i], rnode->children_[i], ret);
              if (ret && OB_PARSER_ERR_SIZE_OVERFLOW == *ret) {
                break;
              }
            }
          }
        }
      }
    }
  }
  return result;
}
// Find by name, return the index of the name if found, otherwise add the name and return the index
int64_t get_question_mark(ObQuestionMarkCtx *ctx, void *malloc_pool, const char *name)
{
  int64_t idx = -1;
  if (OB_UNLIKELY(NULL == ctx || NULL == name)) {
    (void)fprintf(stderr, "ERROR question mark ctx or name is NULL\n");
  } else {
    if (NULL == ctx->name_ && 0 == ctx->capacity_) {
      ctx->capacity_ = MAX_QUESTION_MARK;
      // the errcode will be ignored here. TO BE FIXED.
      ctx->name_ = (char **)parse_malloc(sizeof(char*) * MAX_QUESTION_MARK, malloc_pool);
    }
    if (ctx->name_ != NULL) {
      bool valid_name = true;
      for (int64_t i = 0; valid_name && -1 == idx && i < ctx->count_; ++i) {
        if (NULL == ctx->name_[i]) {
          (void)fprintf(stderr, "ERROR name_ in question mark ctx is null\n");
          valid_name = false;
        } else if (0 == STRCASECMP(ctx->name_[i], name)) {
          idx = i;
        }
      }
      if (-1 == idx && valid_name) {
        if (ctx->count_ >= ctx->capacity_) {
          void *buf = parse_malloc(sizeof(char*) * (ctx->capacity_ * 2), malloc_pool);
          if (OB_UNLIKELY(NULL == buf)) {
            ctx->name_ = NULL;
            (void)printf("ERROR malloc memory failed\n");
          } else {
            MEMCPY(buf, ctx->name_, sizeof(char*) * ctx->capacity_);
            ctx->capacity_ *= 2;
            ctx->name_ = (char **)buf;
          }
        }
        if (ctx->name_ != NULL) {
          int64_t len = 0;
          ctx->name_[ctx->count_] = parse_strdup(name, malloc_pool, &len);
          idx = ctx->count_++;
        }
      }
    } else {
      (void)fprintf(stderr, "ERROR question mark name buffer is null\n");
    }
  }
  return idx;
}

int64_t get_need_reserve_capacity(int64_t n)
{
  int64_t capacity = 0;
   // equal to OB_MALLOC_BIG_BLOCK_SIZE in ob_define.h
  const int64_t max_delta_capacity = (1LL << 21) / sizeof(ParseNode*); // 2MB
  if (n <= 2) {
    capacity = 2;
  } else if ((n & (n - 1)) == 0) {
    capacity = n;
  } else if (n > max_delta_capacity) {
    int64_t i = n / max_delta_capacity;
    capacity = max_delta_capacity * (i + 1);
  } else {
    capacity = 4;
    while (capacity < n) {
      capacity <<= 1;
    }
  }
  return capacity;
}

// (A OR B) OR C --> OR (A, B, C)
ParseNode *push_back_child(void *malloc_pool, int *error_code, ParseNode *left_node, ParseNode *node)
{
  ParseNode *ret_node = NULL;
  if (OB_ISNULL(error_code)) {
    (void)fprintf(stderr, "ERROR parser result is NULL\n");
  } else if (OB_ISNULL(malloc_pool) ||
             OB_ISNULL(left_node) ||
             OB_ISNULL(node) ||
             OB_UNLIKELY(INT64_MAX == left_node->value_)) {
    *error_code = OB_PARSER_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(!(T_OP_OR == left_node->type_ ||
                           T_OP_AND == left_node->type_ ||
                           T_EXPR_LIST == left_node->type_ ||
                           T_SET_UNION == left_node->type_ ||
                           T_SET_UNION_ALL == left_node->type_ ||
                           T_VALUE_VECTOR == left_node->type_ || 
                           T_VALUE_LIST == left_node->type_))) {
    *error_code = OB_PARSER_ERR_UNEXPECTED;
  } else {
    int64_t capacity = get_need_reserve_capacity(left_node->num_child_ + 1);
    if (left_node->value_ < capacity) {
      if (NULL == left_node->children_) {
        left_node->children_ = parse_malloc(capacity * sizeof(ParseNode*), malloc_pool);
      } else {
        left_node->children_ = parse_realloc(left_node->children_, capacity * sizeof(ParseNode*), malloc_pool);
      }
      if (OB_ISNULL(left_node->children_)) {
        *error_code = OB_PARSER_ERR_NO_MEMORY;
      } else {
        left_node->children_[left_node->num_child_] = node;
        left_node->num_child_ = left_node->num_child_ + 1;
        left_node->value_ = capacity;
        ret_node = left_node;
      }
    } else {
      left_node->children_[left_node->num_child_] = node;
      left_node->num_child_ += 1;
      ret_node = left_node;
    }
  }
  return ret_node;
}

// (A OR B) OR (C OR D) --> OR (A, B, C, D)
ParseNode *append_child(void *malloc_pool, int *error_code, ParseNode *left_node, ParseNode *right_node)
{
  ParseNode *ret_node = NULL;
  if (OB_ISNULL(error_code)) {
    (void)fprintf(stderr, "ERROR parser result is NULL\n");
  } else if (OB_ISNULL(malloc_pool) ||
             OB_ISNULL(left_node) ||
             OB_ISNULL(right_node) ||
             OB_UNLIKELY(left_node->type_ != right_node->type_)) {
    *error_code = OB_PARSER_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(!(T_OP_OR == left_node->type_ ||
                           T_OP_AND == left_node->type_ ||
                           T_EXPR_LIST == left_node->type_ ||
                           T_SET_UNION == left_node->type_ ||
                           T_SET_UNION_ALL == left_node->type_))) {
    *error_code = OB_PARSER_ERR_UNEXPECTED;

  } else {
    int64_t num_child = left_node->num_child_ + right_node->num_child_;
    int64_t capacity = get_need_reserve_capacity(num_child);
    if (left_node->value_ < capacity) {
      ParseNode *new_op = new_node(malloc_pool, left_node->type_, capacity);
      if (OB_ISNULL(new_op)) {
        *error_code = OB_PARSER_ERR_NO_MEMORY;
      } else {
        MEMCPY(new_op->children_, left_node->children_, sizeof(ParseNode*) * left_node->num_child_);
        MEMCPY(new_op->children_ + left_node->num_child_, right_node->children_, sizeof(ParseNode*) * right_node->num_child_);
        new_op->num_child_ = num_child;
        new_op->value_ = capacity;
        ret_node = new_op;
      }
    } else {
      MEMCPY(left_node->children_ + left_node->num_child_, right_node->children_, sizeof(ParseNode*) * right_node->num_child_);
      left_node->num_child_ = num_child;
      ret_node = left_node;
    }
  }
  return ret_node;
}
