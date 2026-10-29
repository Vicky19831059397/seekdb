# 文件即数据库：实现进度

状态：开发中的扫描基础库，尚不能作为整题提交。没有使用外部查询引擎。

## 已实现
- CSV：流式记录读取、BOM、UTF-8、LF/CRLF、引号与双引号转义、可配置转义、字段内换行、NULL 与空字符串区分。
- CSV：全量 schema 推断、表头校验、坏行拒绝、批量读取、重扫、单记录/扫描/列数/schema/批量资源限制。
- JSONL：扁平对象、严格 JSON 数字及字符串、Unicode 转义及代理对、首现顺序字段并集、缺失值补 NULL、类型宽化。
- JSONL：拒绝嵌套对象、数组、重复键、非法 UTF-8 和数值溢出，支持全量推断、批量读取及重扫。
- 类型：BIGINT / DOUBLE / BOOLEAN / VARCHAR / DATE / DATETIME / NULL；日期合法性、整数上下界、全 NULL 列。
- CSV 导出序列化后端：可配置分隔符、引号、转义、NULL 文本，区别 NULL、空串及与 NULL 文本相同的普通字符串。
- 文件访问基础：默认关闭、允许目录校验、按已打开句柄核对实际路径、只读常规文件、文件大小上限。
- Windows 拒绝写入/删除共享；Linux/macOS 记录大小/身份/修改时间；每批及重扫前后验证，出错丢弃当前批次。
- 显式 Bazel 测试目标：//unittest/sql/engine/basic:test_file_scan_utils。

## 验证
2026-10-29，在 Windows 用现有 MinGW g++，C++20，-Wall -Wextra -Werror 编译实际基础库源码并执行 standalone 测试：33,980 条断言通过。
符号链接逃逸测试因 Windows 当前权限不允许创建符号链接而跳过，不能声称已验证。
此数量是本地断言数，并非官方用例数或得分。
GoogleTest 入口已准备；官方 Bazel 构建尚未通过，依赖加载耗时，不能标记成功。
POSIX 分支还没有在 Linux/macOS 编译或执行。

复现独立基础测试（仓库根目录）：
```text
g++ -std=c++20 -Wall -Wextra -Werror -DFILE_SQL_STANDALONE_TEST -fexec-charset=UTF-8 -Isrc src/sql/engine/basic/file_scan_utils.cpp src/sql/engine/basic/file_sql_local_file.cpp unittest/sql/engine/basic/test_file_scan_utils.cpp -o file-scan-tests.exe
./file-scan-tests.exe
```

## 尚未实现，必须继续
1. SQL parser/AST：read_csv/read_json/read_ndjson/read_parquet、命名参数、glob、DESCRIBE、COPY。
2. Resolver：动态列元数据、列别名、类型映射；从会话读取 enable_file_sql 与 secure_file_priv。
3. 原生扫描算子、执行计划和重扫，接入投影/过滤/聚合/JOIN；禁止通过导入临时表或外部引擎替代。
4. Parquet footer、row group、编码及 Snappy、可空列与列裁剪。
5. glob 的安全文件发现。
6. COPY 的原生查询执行、目标排他创建、临时文件提交与失败清理。当前只实现行序列化，未实现 COPY 命令。
7. 分配器/OOM 与会话内存预算接入、并发及跨平台专项验证。
8. 官方 parser/utils/mysqltest 公开及隐藏测试。

## 集成边界
TextFileScan 是可测试的文件扫描组件，不是 SQL 执行引擎。LocalFilePolicy 不能由用户 SQL 直接构造绕过会话校验。
基础库 target 暂未链接至服务器执行路径，不暴露任何未鉴权 SQL 功能。
在 SQL 入口、安全门禁和 native operator 完整接通前，不将此分支标记完成，不更新现有 PR #2100，也不保证任何分值。
题面合计为 50 分，但 COPY 段落另有“30 分”字样；按八项 50 分表规划，实际评分待官方确认。
