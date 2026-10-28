# Rust SQL 解析器

使用 `peg 0.8.6`，输入只支持 UTF-8；作为 `rust/` 工作区成员构建。

```sh
./build.py build -j32 seekdb
```

`src/grammar.rs` 定义 SQL、存储过程、内部表达式和统计选项的语法，
`src/node.rs` 提供构树函数。多语句切分复用同一套语法，全文检索布尔表达式在 `src/fts.rs`。
支持 ANSI_QUOTES、NO_BACKSLASH_ESCAPES、PIPES_AS_CONCAT、HIGH_NOT_PRECEDENCE。

语法规则直接构造 CXX 共享节点组成的树，并保留原始字节范围。
`src/bridge.rs` 选择解析入口、处理可执行注释，并按源码顺序为占位符编号。
C++ 在解析内存池中创建现有 `ParseNode` 并复制字符串，共享树由 Rust 释放。
语义检查、名称绑定和执行沿用现有 C++ 引擎；PL 内部 SQL 按语法树绑定过程变量、
收集对象依赖和处理 `SELECT INTO`。

`src/scan.rs` 负责快速参数化、提示过滤和格式化，返回字面量、源码范围及参数化文本；
字符串解码与完整解析共用 `src/lex.rs`。C++ 暂时将字面量记录接入现有参数列表。
命中计划缓存时不再完整解析。
完整解析使用 Rust，失败直接报错，不回退到旧解析器。SQL、PL 和全文检索的
Bison/Flex 实现及生成工具依赖均已移除。

TODO：对照历史 Bison/Flex 规则审计语法覆盖，并补充组合差分验证。
