# Rust SQL 解析器

使用 `peg 0.8.6`，语法在 `src/grammar.rs`，构树函数在 `src/node.rs`。
输入只支持 UTF-8；作为 `rust/` 工作区成员构建，没有独立 CLI 或 JSON 中间格式。

## 迁移验证入口

```sh
./build.py build -j16 --//rust:enable_rust_parser=true seekdb
```

启用后，普通 SQL 和内部 SQL 的完整解析阶段使用 Rust，解析失败直接报错。
快速参数化、多语句切分和 PL SQL 预处理仍沿用旧实现。
实验开关和旧解析链路在全量验证通过后删除。

已接入基本 SELECT、表和列别名、DISTINCT、表达式、WHERE、ORDER BY、LIMIT，
以及 INNER／LEFT／RIGHT／FULL／CROSS JOIN、ON／USING、普通函数调用、
COUNT／SUM／MIN／MAX／AVG、IF／ISNULL、时间函数调用、GROUP BY／HAVING、标量子查询、EXISTS、
IN 子查询和派生表。语义检查、相关子查询绑定与执行沿用现有 C++ 引擎。
支持 ANSI_QUOTES、NO_BACKSLASH_ESCAPES、PIPES_AS_CONCAT、HIGH_NOT_PRECEDENCE。

已接入多行 INSERT、INSERT SELECT、INSERT SET、REPLACE、冲突更新、UPDATE、DELETE
和事务提交／回滚、保存点；基础建删库表、USE、常用列类型、默认值、自增、列唯一约束和主键。
支持 CASE、系统／用户变量、SET、字符集设置，以及基础 SHOW／DESCRIBE。
已接入 CAST／CONVERT、二进制字面量、行锁、VALUES、LATERAL、排名和聚合窗口函数、
命名窗口和数值窗口边界，以及启动和 schema 刷新使用的 ALTER SYSTEM 和基础 hint。
支持基本账户创建／删除、改密、授权／撤销和 SHOW GRANTS，以及 SQL PREPARE／EXECUTE。
已接入存储过程／函数的创建、调用和执行时解析，以及系统包声明和包体解析。
支持参数、变量、条件、循环、游标、异常处理、GET DIAGNOSTICS、SELECT INTO、限定名称函数调用和 JSON_TABLE。
内部 SQL 的命名参数保留首次出现顺序，重复引用共用参数编号。
支持 ANALYZE TABLE、统计采集的近似去重和直方图聚合，以及全表扫描和索引 hint。
支持 UNION／UNION ALL、INTERSECT、EXCEPT／MINUS，以及括号内外各自的排序和 LIMIT。
支持普通／唯一索引、前缀索引、STORING、LOCAL／GLOBAL、KEY／HASH 分区和基础表选项，
以及 EXPLAIN、索引回填使用的快照查询、并行 DML 和动态采样 hint。
支持列和索引的基础 ALTER TABLE 操作、重命名表、TRUNCATE、SHOW INDEX 和 COLLATE 表达式。
支持视图创建／替换／修改、CREATE TABLE AS／LIKE、列字符集、GROUP_CONCAT（含窗口用法），
以及 FROM DUAL 和常用 EXPLAIN 格式。
支持生成列、CHECK 约束、LIST 分区、指定分区读写和表级索引 hint；
支持 FIRST_VALUE／LAST_VALUE／NTH_VALUE／LEAD／LAG、NULL 处理选项、LIKE ESCAPE 和用户变量赋值。
支持 DATE_ADD／DATE_SUB／ADDDATE／SUBDATE、TIMESTAMPADD／TIMESTAMPDIFF、EXTRACT 和 MySQL 可执行版本注释。
支持日期时间字面量、REGEXP／RLIKE、SELECT ALL、RANGE 分区、ROW_FORMAT、数据库字符集选项、
SHOW CHARSET／COLLATION、SET TRANSACTION 和 MAJOR／MINOR FREEZE。
支持模板和逐分区定义的二级分区、ANY／SOME／ALL 子查询、SQL_CALC_FOUND_ROWS，
以及分布式扫描、聚合、集合运算和常用查询改写的 outline 提示。
支持 POSITION／TRIM／WEIGHT_STRING、IS UNKNOWN、GROUP BY 排序和 ROLLUP、ENUM／SET，
以及 UTF-8／二进制字符串前缀、相邻字符串、列 ON UPDATE、表字符集转换和 TABLE(GENERATOR(...))。
支持外键声明、增删约束和引用动作，以及 STRING、TINY／MEDIUM 文本和二进制大对象类型。
支持数组、向量、MAP 和稀疏向量类型，数组字面量、lambda、集合聚合与向量距离函数；
支持 APPROX LIMIT 和查询参数、函数索引、括号 JOIN，以及标准差、方差和位聚合。
系统视图及统计采样支持 CAST 的 IGNORE 和 CHARSET 选项；未知优化器 hint 按原有规则忽略。
这些语句与 SELECT 共用 `ParseTree` 接口。

其他聚合函数和特殊函数语法、CTE、其他窗口函数、其他 DDL、完整 hint 语法、
其余 PL 语法尚未迁移。系统包和 mysqltest 初始化已通过，全量用例尚未通过；
当前不是默认解析器的全量替换。

## 跨语言接口

语法规则直接构造 CXX 共享节点组成的树，保留原始字节范围。没有独立 AST 转换层
或节点数组。`src/bridge.rs` 只负责调用语法和按源码顺序给占位符编号。
C++ 用通用遍历在解析内存池中创建现有 `ParseNode`，字符串复制到该内存池。
共享树由 Rust 自动释放。

沿用快速参数化和计划缓存；命中缓存时不再次完整解析。转换保留常量原文、位置、
占位符的源码顺序和投影原文，包括嵌套查询和逗号形式的 LIMIT。
语法嵌套和表达式深度限制为 128 层。
