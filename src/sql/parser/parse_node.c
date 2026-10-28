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
#include "sql/parser/parse_define.h"
#include "sql/parser/parse_malloc.h"
#include <stdio.h>
#include <ctype.h>

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
