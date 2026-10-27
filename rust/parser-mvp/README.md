# rust-peg SQL 解析原型

使用 `peg 0.8.6`，编译时生成解析代码，返回项目自己的 AST。
输入只支持 UTF-8。语法在 `src/grammar.rs`，AST 在 `src/ast.rs`。

独立解析：

```sh
cargo run --release --manifest-path rust/parser-mvp/Cargo.toml -- \
  'SELECT id, price * 2 AS total FROM orders WHERE id = 42'
```

`--expr` 解析表达式；`--repeat N` 测量解析和 AST 构造、销毁，不包含 JSON 输出。

## 数据库接入

```sh
./build.py build -j16 --//rust:enable_parser_mvp=true seekdb
```

在这个二进制启动的实例中，在 SELECT 后加 `/*+parser_mvp*/` 的 SQL 在完整解析阶段使用
rust-peg；mysql CLI 需要 `--comments` 保留标记。普通查询和内部 SQL 仍使用旧解析器。
标记采用 hint 注释位置，规范化 SQL 时保留，因此 prepared statement 的 prepare 和首次生成计划阶段都能使用新解析器。

```sql
SELECT /*+parser_mvp*/ id + 10 AS n FROM t WHERE id = 1 ORDER BY id LIMIT 2;
```

已接入表、列、别名、通配符、DISTINCT、常量、占位符、算术和比较表达式、
AND/OR/NOT、IN 列表、BETWEEN、IS NULL、LIKE、WHERE、ORDER BY、LIMIT。
支持 ANSI_QUOTES、NO_BACKSLASH_ESCAPES、PIPES_AS_CONCAT、HIGH_NOT_PRECEDENCE。

独立语法还覆盖 JOIN、函数调用、子查询、GROUP BY 和 HAVING，但这些尚未接入
数据库桥接；完整解析遇到它们时明确报错。DDL、写入、UNION、CTE、窗口函数、
hint、版本注释和存储程序也尚未迁移。这不是默认解析器的全量替换。

## 接口与计划缓存

Rust AST 保留表达式和标识符的原始字节范围。`src/bridge.rs` 按子节点在前、父节点
在后的顺序输出节点数组；CXX 一次返回，C++ 在查询 arena 中创建现有 `ParseNode`。
字符串复制到 arena，C++ 不持有 Rust 临时字符串；节点编号仅在这次转换中有效。

沿用现有快速参数化、计划缓存、语义分析和执行器。缓存命中时不会再次完整解析。
转换保留常量原文、位置、占位符顺序和投影原文；逗号形式的 LIMIT 保持 offset、count
的源码顺序，由现有参数化逻辑确定哪些值可以替换。

当前原型限制语法嵌套和表达式树深度，表达式最多 128 层。Rust 在有界栈上运行，
适配 seekdb 的 SMART_CALL 栈切换。后续扩展语法时，需要一并核对系统的表达式限制。

实验开关和请求标记仅服务迁移验证；全量切换时与旧实现一起删除。测试和测量程序
留在仓库外，不随此原型提交。
