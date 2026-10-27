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

pub mod ast;
#[cfg(feature = "server")]
mod bridge;
mod grammar;

use ast::{Expr, Select};

#[derive(Clone, Copy, Default)]
pub struct SqlMode {
    pub ansi_quotes: bool,
    pub no_backslash_escapes: bool,
    pub pipes_as_concat: bool,
    pub high_not_precedence: bool,
}

#[derive(Default)]
pub struct Parser {
    mode: SqlMode,
}

impl Parser {
    pub fn new(mode: SqlMode) -> Self {
        Self { mode }
    }

    pub fn parse(&self, sql: &str) -> Result<Select, peg::error::ParseError<peg::str::LineCol>> {
        grammar::sql::query(sql, self.mode)
    }

    pub fn parse_expression(
        &self,
        sql: &str,
    ) -> Result<Expr, peg::error::ParseError<peg::str::LineCol>> {
        grammar::sql::expression_input(sql, self.mode)
    }
}
