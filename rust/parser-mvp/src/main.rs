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

use parser_mvp::Parser;
use std::{hint::black_box, io::Read, process::ExitCode, time::Instant};

fn run() -> Result<(), String> {
    let mut args = std::env::args().skip(1);
    let mut expression = false;
    let mut repeat = None;
    let mut sql = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--help" | "-h" => {
                println!("parser-mvp [--expr] [--repeat N] [SQL]\nReads SQL from stdin when omitted. Prints an owned AST as JSON.\n--repeat measures matching + AST construction + destruction, excluding JSON output.");
                return Ok(());
            }
            "--expr" => expression = true,
            "--repeat" => {
                let count = args
                    .next()
                    .ok_or("--repeat requires a positive integer")?
                    .parse::<usize>()
                    .map_err(|_| "--repeat requires a positive integer")?;
                if count == 0 {
                    return Err("--repeat requires a positive integer".into());
                }
                repeat = Some(count);
            }
            _ if sql.is_none() => sql = Some(arg),
            _ => return Err("Pass SQL as one quoted argument, or use stdin".into()),
        }
    }
    let sql = match sql {
        Some(sql) => sql,
        None => {
            let mut sql = String::new();
            std::io::stdin()
                .read_to_string(&mut sql)
                .map_err(|e| e.to_string())?;
            sql
        }
    };
    let parser = Parser::default();
    if let Some(count) = repeat {
        // Validate and warm the same complete path that is measured below.
        if expression {
            black_box(
                parser
                    .parse_expression(&sql)
                    .map_err(|error| error.to_string())?,
            );
        } else {
            black_box(parser.parse(&sql).map_err(|error| error.to_string())?);
        }
        let start = Instant::now();
        for _ in 0..count {
            if expression {
                black_box(
                    parser
                        .parse_expression(black_box(&sql))
                        .map_err(|error| error.to_string())?,
                );
            } else {
                black_box(
                    parser
                        .parse(black_box(&sql))
                        .map_err(|error| error.to_string())?,
                );
            }
        }
        let elapsed = start.elapsed();
        println!(
            "{}",
            serde_json::json!({
                "iterations": count, "successful_parses": count, "sql_bytes": sql.len(),
                "total_ms": elapsed.as_secs_f64() * 1e3,
                "us_per_parse": elapsed.as_secs_f64() * 1e6 / count as f64,
            })
        );
    } else {
        let output = if expression {
            serde_json::to_string_pretty(
                &parser
                    .parse_expression(&sql)
                    .map_err(|error| error.to_string())?,
            )
        } else {
            serde_json::to_string_pretty(&parser.parse(&sql).map_err(|error| error.to_string())?)
        };
        println!("{}", output.map_err(|e| e.to_string())?);
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}
