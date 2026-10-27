# Rust SQL 解析器

使用 `peg 0.8.6`，语法在 `src/grammar.rs`，构树函数在 `src/node.rs`。
输入只支持 UTF-8；作为 `rust/` 工作区成员构建，没有独立 CLI 或 JSON 中间格式。

## 迁移验证入口

```sh
./build.py build -j16 --//rust:enable_rust_parser=true seekdb
```

在语句首个关键字后加 `/*+parser_mvp*/`，完整解析阶段即使用 Rust 解析器。
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

已接入多行 INSERT、INSERT SELECT、INSERT SET、REPLACE、冲突更新、UPDATE、DELETE
和事务提交／回滚；基础建删库表、USE、常用列类型、默认值、自增、列唯一约束和主键。
这些语句与 SELECT 共用 `ParseTree` 接口。

限定名称的函数调用、其他聚合函数和特殊函数语法、UNION、CTE、窗口函数、
其他 DDL、会话设置、hint、版本注释和存储程序尚未迁移。当前不是默认解析器的全量替换。

## 跨语言接口

语法规则直接构造 CXX 共享节点组成的树，保留原始字节范围。没有独立 AST 转换层
或节点数组。`src/bridge.rs` 只负责调用语法和按源码顺序给占位符编号。
C++ 用通用遍历在解析内存池中创建现有 `ParseNode`，字符串复制到该内存池。
共享树由 Rust 自动释放。

沿用快速参数化和计划缓存；命中缓存时不再次完整解析。转换保留常量原文、位置、
占位符的源码顺序和投影原文，包括嵌套查询和逗号形式的 LIMIT。
语法嵌套和表达式深度限制为 128 层，Rust 使用有界栈以适配 SMART_CALL 栈切换。
