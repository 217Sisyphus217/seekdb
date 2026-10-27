# Rust SQL 解析器

使用 `peg 0.8.6`，语法在 `src/grammar.rs`，项目自己的 AST 在 `src/ast.rs`。
输入只支持 UTF-8；作为 `rust/` 工作区成员构建，没有独立 CLI 或 JSON 中间格式。

## 迁移验证入口

```sh
./build.py build -j16 --//rust:enable_rust_parser=true seekdb
```

在 SELECT 后加 `/*+parser_mvp*/`，完整解析阶段即使用 Rust 解析器。
mysql CLI 需要 `--comments` 保留标记。普通查询和内部 SQL 仍使用旧解析器；
Rust 解析失败直接报错。标记保留在规范化 SQL 中，prepared statement 的 prepare
和首次生成计划阶段均使用 Rust。实验开关和标记在全量切换时删除。

```sql
SELECT /*+parser_mvp*/ a.id, count(b.id) AS n
FROM t a LEFT JOIN u b ON a.id = b.id
GROUP BY a.id HAVING count(b.id) > 0 ORDER BY a.id;
```

已接入基本 SELECT、表和列别名、DISTINCT、表达式、WHERE、ORDER BY、LIMIT，
以及 INNER／LEFT／RIGHT／CROSS JOIN、ON／USING、普通函数调用、
COUNT／SUM／MIN／MAX／AVG、IF／ISNULL、时间函数调用、GROUP BY／HAVING、标量子查询、EXISTS、
IN 子查询和派生表。语义检查、相关子查询绑定与执行沿用现有 C++ 引擎。
支持 ANSI_QUOTES、NO_BACKSLASH_ESCAPES、PIPES_AS_CONCAT、HIGH_NOT_PRECEDENCE。

限定名称的函数调用、其他聚合函数和特殊函数语法、UNION、CTE、窗口函数、
DDL、写入、hint、版本注释和存储程序尚未迁移。当前不是默认解析器的全量替换。

## 跨语言接口

Rust AST 保留表达式和标识符的原始字节范围。`src/bridge.rs` 输出子节点在前、
父节点在后的节点数组；CXX 一次返回，C++ 在解析内存池中创建现有 `ParseNode`。
字符串复制到内存池，C++ 不持有 Rust 临时字符串。

沿用快速参数化和计划缓存；命中缓存时不再次完整解析。转换保留常量原文、位置、
占位符的源码顺序和投影原文，包括嵌套查询和逗号形式的 LIMIT。
语法嵌套和树深度限制为 128 层，Rust 使用有界栈以适配 SMART_CALL 栈切换。
