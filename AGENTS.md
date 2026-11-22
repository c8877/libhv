# AGENTS.md

This file provides guidance to agents when working with code in this repository.

## Project Overview

libhv is a cross-platform C/C++ network library providing event-loop with non-blocking IO and timer. Core is C99, high-level wrappers are C++11. Compatible with gcc4.8+, MSVC2015+, clang. Supports Linux, Windows, macOS, Android, iOS, BSD, Solaris.

## Build Commands

### Makefile (primary, Unix)
```bash
./configure --with-openssl --with-http --with-mqtt --with-kcp  # configure options
make libhv          # build library only (shared + static)
make                # build library + examples
make examples       # build all example programs
make unittest       # compile unit tests
make evpp           # build C++ evpp tests (requires libhv built first)
make clean          # clean build artifacts
sudo make install   # install to /usr/local/include/hv and /usr/local/lib
```

### CMake (cross-platform)
```bash
mkdir build && cd build
cmake .. -DWITH_OPENSSL=ON -DWITH_HTTP=ON -DBUILD_EXAMPLES=ON
cmake --build .
# Windows: cmake .. -G "Visual Studio 17 2022" -A x64
```

### Bazel
```bash
bazel build libhv
```

### Package Managers
```bash
vcpkg install libhv    # vcpkg
xrepo install libhv    # xmake
```

## Testing

```bash
make unittest                 # compile all unit tests
make run-unittest             # compile and run unit tests (calls scripts/unittest.sh)
bash scripts/unittest.sh      # run pre-built unit tests
make check                    # integration test: builds httpd, runs HTTP checks (scripts/check.sh)
```

Run a single unit test directly:
```bash
bin/rbtree_test    # or any test binary in bin/
```

Run evpp C++ tests (link against libhv):
```bash
make evpp
bin/TcpServer_test
bin/EventLoop_test
```

## Key Configuration Options

Build flags via `./configure` or CMake `-D` options (see `config.ini` for defaults):

| Makefile flag | CMake flag | Purpose |
|---|---|---|
| `--with-openssl` | `-DWITH_OPENSSL=ON` | SSL/TLS via OpenSSL |
| `--with-gnutls` | `-DWITH_GNUTLS=ON` | SSL/TLS via GnuTLS |
| `--with-mbedtls` | `-DWITH_MBEDTLS=ON` | SSL/TLS via mbedTLS |
| `--with-nghttp2` | `-DWITH_NGHTTP2=ON` | HTTP/2 support |
| `--with-kcp` | `-DWITH_KCP=ON` | KCP reliable UDP |
| `--with-mqtt` | `-DWITH_MQTT=ON` | MQTT client |
| `--with-protocol` | `-DWITH_PROTOCOL=ON` | ICMP, DNS, FTP, SMTP |
| `--with-evpp` | `-DWITH_EVPP=ON` | C++ wrappers (default: yes) |
| `--without-evpp` | `-DWITH_EVPP=OFF` | Pure C build, no C++ |
| `--enable-uds` | `-DENABLE_UDS=ON` | Unix Domain Socket |
| `--with-io-uring` | `-DWITH_IO_URING=ON` | io_uring event backend (Linux 5.1+) |

## Architecture

```
Application / Examples
    │
    ├── http/server, http/client   HTTP/WebSocket/gRPC (C++)
    ├── mqtt/                      MQTT client (C)
    ├── protocol/                  ICMP, DNS, FTP, SMTP (C)
    │
    ├── evpp/                      C++ wrappers: TcpServer, TcpClient, UdpServer, EventLoop
    │
    ├── event/                     Core event loop: hloop, hio, htimer
    │   └── backends: epoll (Linux), kqueue (macOS/BSD), iocp/wepoll (Windows), io_uring (Linux 5.1+), select (fallback)
    │   └── kcp/                   KCP reliable UDP transport
    │
    ├── ssl/                       Unified SSL interface (OpenSSL / GnuTLS / mbedTLS / platform)
    │
    ├── base/                      Platform abstraction, sockets, threads, logging, data structures
    ├── util/                      C utilities (base64, md5, sha1)
    └── cpputil/                   C++ utilities (string, path, file, json, threadpool, ini parser)
```

**Layering rules**: `base/` has no dependencies on other modules. `event/` depends on `base/` and `ssl/`. `evpp/` wraps `event/`. `http/` depends on `evpp/`. Higher layers are optional and controlled by build flags.

**Public API entry point**: `hv.h` includes all base headers. Module-specific headers (e.g., `HttpServer.h`, `TcpServer.h`, `mqtt_client.h`) are the primary include for each feature. Installed headers go to `include/hv/`.

**Key types**: `hloop_t` (event loop), `hio_t` (IO handle), `htimer_t` (timer) in the C API. `EventLoop`, `TcpServer`, `TcpClient`, `Channel` in the C++ API. `HttpRequest`, `HttpResponse`, `HttpService` for HTTP.

## Code Style

- **Formatting**: `.clang-format` — LLVM-based, 4-space indent, 160 column limit, pointer right-aligned, `catch`/`else` on new line (custom brace wrapping), no include sorting.
- **C API naming**: `h` prefix for functions (`hloop_new`, `hio_read`), `_t` suffix for types (`hloop_t`, `hio_t`), UPPERCASE macros (`HV_EXPORT`).
- **C++ API naming**: PascalCase classes (`EventLoop`, `TcpServer`), `hv` namespace.
- **File naming**: lowercase with underscores (`.c` for C, `.cpp` for C++).
- **Platform-specific code**: isolated via `hplatform.h` and `#ifdef` conditional compilation.

## CI

GitHub Actions (`.github/workflows/CI.yml`): builds and tests on Linux (with OpenSSL+nghttp2+KCP+MQTT), Windows (CMake + VS2022), macOS, Android (NDK cross-compile), iOS (Xcode cross-compile). Benchmark workflow runs echo-server throughput and HTTP performance comparisons.

## rtunnel 部署

部署步骤、目标机、验证与回滚写在 `rtunnel/deploy/DEPLOY.md`，动手前先读，不要凭记忆重编。

- 测试环境用直推：板子跑 `install.sh`，relay 换静态二进制 + `admin/web` 目录。**直推不需要 `RT` / `ATK`**；只有「上传文件仓库 → OTA 分发」才需要令牌，别把直推当成缺凭据。
- 部署前先找本机已有的脚本和通道（`/tmp/*.py`、免密 SSH、`relay.opts`），找不到再问；不要用"需要你提供……"收尾。
- 上传文件仓库用 `deploy/upload_agent.sh`；atk 内置在该脚本的默认值里，轮换后改那一行，需要新的就直接开口要。
- 汇报只说结果：版本、服务状态、隧道是否 ESTAB、线程/RSS。不要复述步骤清单。

## 前端（管理台与设备本地页）组件规则

管理台在 `rtunnel/relay/admin/web`（Vue 3 + Element Plus 2.x）。设备本地页与管理台共用
`src/components/DeviceView.vue`，由 `src/local` 入口构建成单文件 HTML 内置到 agent，
不再单独维护一套页面。**Element 已有的能力必须直接用，禁止再实现一份**；框架不满足时，用它的扩展点（插槽、`sortable="custom"` + `@sort-change`、`el-table` 的 `#empty` 等）扩展，不要另写一套替换。

| 需求 | 必须用 | 禁止 |
|---|---|---|
| 数据表 | `el-table` / `el-table-column` | 手写 `<table>`、表头点击自排序、自绘排序箭头 |
| 服务端分页/排序 | `sortable="custom"` + `@sort-change` | 前端只排当前页、自己维护排序状态 |
| 表单校验 | `el-form` + `rules` + `validate()` | 提交前手写 `if (!x) ElMessage.warning()` |
| 输入 / 下拉 / 开关 | `el-input` / `el-select` / `el-switch` / `el-date-picker` | 原生 `<input>` / `<select>` |
| 按钮 / 选择 | `el-button` / `el-checkbox` / `el-radio-group` | 原生 `<button>`、`div role="button"`、伪 checkbox |
| 弹层 | `el-dialog` / `el-drawer` / `el-popover` / `ElMessageBox` | 手写遮罩、`position: fixed` 弹层 |
| 分页 | `el-pagination` | 自绘页码 |
| 进度 / 标签页 / 分段 | `el-progress` / `el-tabs` / `el-segmented` | 自绘进度条、tab 按钮、切换按钮组 |
| 菜单 | `el-menu` / `el-dropdown` + `el-dropdown-menu` | 手写列表菜单；光标定位的右键菜单 Element 无原生支持，自建需标注 |
| DOM | 模板 + 响应式状态 | `document.createElement` / `innerHTML` / `insertAdjacentHTML` |

- **体积不是复制实现的理由**。需要更小的部署包时发 lite 变体（`rtunnel-agent-noweb`，不含本地页），不要为体积另写一套页面、表格或组件，也不要靠构建裁剪维护第二份源码。功能差异只放在构建入口、路由和权限层。
- 改完管理台或设备页必须跑 `cd rtunnel/relay/admin/web && npm run check:ui`；`npm run build`、`npm run dev`、`npm run build:local` 已内置，违反会直接失败。
- Element 确实没有对应能力时才允许自建，并写清原因，否则检查失败：
  `<!-- ui-allow: 原因 -->`（单行，可写在标签上一行）或 `<!-- ui-allow-begin: 原因 --> … <!-- ui-allow-end -->`（成块）；脚本区用 `// ui-allow: 原因`。
- 表格排序、表单校验、弹层、tab、分页不属于可放行范围。
- 设备本地页不要另写页面：管理台与本地页共用 `components/DeviceView.vue`，差异只通过 `mode`、API 适配（`src/local/api.js`）和资源权限处理。`npm run build:local` 生成 `rtunnel/agent/web/index.html`，这是构建产物，不要手改。
- `shared/web/*.js`（`fs-ui` / `svc-ui` / `storage-ui` / `table-ui`）只作为 Vue 卡片/组件内部的渲染内核；不要再基于它们写页面级表格、表单或弹层。

