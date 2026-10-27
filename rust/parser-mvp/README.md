# 运行时 PEG 解析原型

验证语法作为数据、运行时解释执行的方案。独立 Cargo workspace，尚未接入数据库。

```bash
cargo run --release --manifest-path rust/parser-mvp/Cargo.toml -- \
  'SELECT id, price * 2 AS total FROM orders WHERE id = 42'
```

输出 JSON 语法树。省略 SQL 参数时从标准输入读取；`--expr` 单独解析表达式。
重复解析同一输入可使用 `--repeat 10000`，输出初始化时间和每次解析耗时。

## 实现与范围

`src/sql.pest` 通过 `include_str!` 嵌入二进制。`Parser::new()` 调用
`pest_meta` 校验、优化语法，并创建 `pest_vm::Vm`。调用方创建一次 Parser 后复用；
`parse(&self, sql)` 使用独立匹配状态，成功后转换为持有自身字符串的 AST。
语法规则没有副作用，失败分支不修改最终 AST。没有 SQL 解析器代码生成步骤。

覆盖 SELECT、DISTINCT、别名、表连接、WHERE、GROUP BY、HAVING、ORDER BY、LIMIT、
子查询、函数调用，以及算术、逻辑、比较、IN、BETWEEN、LIKE、IS 运算。
表达式优先级通过分层规则表达；其余语法主要在 SELECT 和表引用规则中。

这是固定默认语义的 UTF-8 子集：支持 ASCII 未引用标识符、反引号标识符、单引号字符串、
有限的反斜杠转义和普通注释。关键词分类也仅覆盖该子集。
DDL/DML、PL、全文检索、UNION/CTE、完整 MySQL 方言、SQL mode、非 UTF-8 输入、hint、
版本注释、快速参数化和计划缓存均不在本轮范围内。AST 没有绑定表列或做类型检查。

`pest_vm` 用于快速验证运行时方案，不代表最终引擎选型；它也不是 DuckDB 的实现。

## 本机测量

Linux x86_64，Xeon Platinum 8369B，rustc 1.91.1；pest 系列固定 2.9.5。
Release 使用默认 opt-level=3、thin LTO、codegen-units=1，并保留调试信息。
时间均为 5 轮的中位数及范围，单线程解析；不同查询样例交替执行。

| 工作 | 中位数 | 范围 |
|---|---:|---:|
| 干净构建，包含依赖编译，`-j4` | 17.44 s | 16.75–17.95 s |
| 修改语法文本后的构建 | 8.38 s | 7.90–8.66 s |
| 初始化语法 | 21.77 ms | 21.15–22.59 ms |
| `SELECT 1` | 33.94 µs/次 | 33.39–34.63 µs |
| `SELECT id FROM users WHERE id=42` | 98.67 µs/次 | 95.29–109.19 µs |
| JOIN、分组、排序样例 | 529.55 µs/次 | 521.72–716.05 µs |

JOIN 样例：

```sql
SELECT u.id, count(o.id) AS n
FROM users u LEFT JOIN orders o ON u.id=o.uid
WHERE u.id BETWEEN 1 AND 100 AND o.id IS NOT NULL
GROUP BY u.id HAVING count(o.id)>2
ORDER BY n DESC LIMIT 20 OFFSET 5
```

解析计时包含匹配、AST 构造和销毁，排除语法初始化、JSON 序列化和进程启动。
每轮分别执行 20,000 / 10,000 / 2,000 次，全部成功。构建使用已下载依赖、离线模式和
不同的空 target 目录；OS 文件缓存未清除。语法修改测量追加注释，依赖编译产物保留；
仍需重编原型 crate 并链接。第一轮干净构建的开始阶段与独立 CPU 采样短暂重叠。

独立分配计数测得共享语法保留约 69.3 KiB；三个样例的额外堆峰值分别约
6.9 / 9.9 / 28.4 KiB，AST 销毁后净新增分配归零。这是 Rust 请求的堆字节数，
不包含栈、分配器元数据或原生代码的内部申请，不等同于进程 RSS。

独立 perf 采样中，约 26.9% 样本位于 VM 表达式解释、25.6% 位于 HashMap 查找、
10.6% 位于规则解释。没有使用采样运行的耗时作为上表数据。

验证包括 167 项检查（含与 Python 算术语法树对照）、4 线程共享语法生成 400 棵独立 AST、
100/500/2,000 个未闭合括号的错误输入，以及 `cargo clippy --release -- -D warnings`。
测试、分配计数器和测量脚本保留在仓库外，未提交。

结论：运行时语法与无副作用 AST 转换已跑通，外部 Bison/Flex 不是这个原型的构建依赖。
通用 VM 的规则解释和查表成本明显；进一步选型应评估初始化时解析规则引用、减少运行时查表的引擎。
当前只有语法子集，这些数字不能用来声称比现有完整 SQL 解析器更快。
