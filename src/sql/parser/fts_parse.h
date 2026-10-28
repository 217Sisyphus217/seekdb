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

#pragma once

#include <cstdint>

enum FtsError {
  FTS_OK = 0,
  FTS_ERROR_MEMORY = 1,
  FTS_ERROR_SYNTAX = 2,
  FTS_ERROR_OTHER = 3,
};

enum FtsNodeType {
  FTS_NODE_OPER,               /*!< Operator */
  FTS_NODE_TERM,               /*!< Term (or word) */
  FTS_NODE_LIST,               /*!< Expression list */
  FTS_NODE_SUBEXP_LIST         /*!< Sub-Expression list */
};

enum FtsOperType {
  FTS_NONE, /*!< No operator */

  FTS_IGNORE, /*!< Ignore rows that contain
              this word */

  FTS_EXIST, /*!< Include rows that contain
             this word */
};

typedef struct FtsString{
  char *str_;
  uint32_t len_;
} FtsString;

typedef struct FtsNode FtsNode;

typedef struct {
  FtsNode *head; /*!< Children list head */
  FtsNode *tail; /*!< Children list tail */
} FtsNodeList;


struct FtsNode{
  enum FtsNodeType type;        /*!< The type of node */
  FtsString term;        /*!< Term node */
  enum FtsOperType oper;        /*!< Operator value */
  FtsNodeList list;        /*!< Expression list */
  FtsNode *next;       /*!< Link for expr list */
  FtsNode *last; /*!< Direct up node */
  bool go_up;              /*!< Flag if go one level up */
};

typedef struct {
  int     ret_;
  FtsString err_info_;
  FtsNode *root_;
} FtsParserResult;

void fts_parse_docment(const char *input, int length, void *pool, FtsParserResult *result);
