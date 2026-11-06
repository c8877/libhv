# sqlite3（SQLite amalgamation）

- 版本：SQLite 3.51.0（`sqlite3.h` 中 `SQLITE_VERSION "3.51.0"`）
- 来源：<https://www.sqlite.org/2025/sqlite-amalgamation-3510000.zip>
- 文件：`sqlite3.c`、`sqlite3.h`、`sqlite3ext.h`（未收录 `shell.c`，产物不需要命令行工具）
- 许可：公有领域（SQLite 作者声明 <https://www.sqlite.org/copyright.html>）

## 用途

只在 `--iox --iox-admin` 时随产物分发到 `<out>/3rd/sqlite3/`：admin 的预设模板库
（`src/<dir>/iox/admin/tpl_store.cpp`）用 SQLite 存「端点 / 消息 / 字段」三级模板。
生成树的 `CMakeLists.txt` 会把它编译成 `{out}_sqlite3` 静态库并链进 `{out}_iox_admin`；
消费方不需要自带 sqlite3。

用 `--no-vendor-deps` 时不会复制本目录，产物改为从 `--incdir/--libdir`
（`CMAKE_INCLUDE_PATH` / `CMAKE_LIBRARY_PATH`）查找已安装的 `sqlite3.h` / `libsqlite3`；
找不到则告警并跳过 `_iox_admin` 目标，不影响主库。
