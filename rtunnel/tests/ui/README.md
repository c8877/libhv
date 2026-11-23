# 设备视图 UI 测试

管理台设备详情页和设备本地页的端到端测试。用 Playwright 驱动 headless Chromium，
直连本机起的 relay + agent，不需要部署、不需要令牌。

## 前置

```bash
make libhv                 # 或 cmake --build build
cd rtunnel/relay/admin/web && npm run build   # 管理台 dist，run-ui.sh 会让 relay 直接托管
npm i -g playwright && npx playwright install chromium
```

`run-ui.sh` 会自己检查二进制、dist、node、playwright，缺什么直接报出来。

## 用法

```bash
rtunnel/tests/ui/run-ui.sh                  # 跑全部
rtunnel/tests/ui/run-ui.sh admin/smoke      # 跑单个
rtunnel/tests/ui/run-ui.sh local/page local/metrics
rtunnel/tests/ui/run-ui.sh list             # 列出用例
UI_KEEP=1 rtunnel/tests/ui/run-ui.sh admin/forms   # 保留临时目录
```

失败时会打印临时目录，里面有 `relay.log` / `agent.log` / `agent2.log` 和
`artifacts/` 下的截图。成功默认清理。

## 用例

| 用例 | 覆盖 |
|---|---|
| `admin/smoke` | 9 个主 tab、概览 KPI、摘要吸顶、网络/存储/文件/服务/进程/监控/串口/能力、电源确认框、手机布局 |
| `admin/tables` | 10 个列表页表格渲染与表头排序、分发/OTA 表排序、设备网卡表 |
| `admin/proc-net` | 进程列服务端排序、TCP/UNIX/IPC 服务端排序、任务列表与 OTA 任务排序 |
| `admin/files` | 文件列表排序、部署到设备（el-dialog/el-form/进度）、落盘确认 |
| `admin/services` | 服务列表重启次数列排序（宿主无 systemd 时跳过） |
| `admin/forms` | 登录必填、移动端 el-drawer、账号/证书/字典表单校验、权限组 el-menu |
| `admin/metrics` | 添加进程指标、chip 与曲线、刷新保持选择、取消 chip 停采样、双设备隔离 |
| `local/page` | 本地页 9 个 tab、进程/TCP 排序、监控曲线、串口与能力只读、文件编辑器、存储、服务 |
| `local/metrics` | 本地页进程 chip 默认选中、可隐藏、采样配置只读 |

## 环境

`run-ui.sh` 启动：

- relay：随机端口，`--admin-docroot` 指向管理台 dist，`--admin-file-dir` 放 `a.txt` / `b.txt`
- agent `uitest`：带本地页（随机端口）、文件读写、服务读写，采样配置含 `cpu/mem/load/proc:1:*`
- agent `uitest2`：第二台设备，只用于验证指标不串设备

采样配置通过管理台 API 写进 relay 的库。**relay 的库是采样配置的准**，agent 配置文件
只是首次/离线默认值；测试里要改默认指标就改 `run-ui.sh` 里那段 PUT。

脚本还往 `distribution_tasks` 插入 5 行，供任务列表和 OTA 页排序断言使用。

测试通过 `lib/harness.js` 读取 `A`（管理台）、`L`（本地页）、`DEV` / `DEV2`、`T`、`UI_ARTIFACTS`。

## 新增用例

1. 在 `admin/` 或 `local/` 下新建 `.js`，`require('../lib/harness')`；
2. 用 `h.check(name, ok, extra)` 断言，`h.finish()` 汇总退出码；
3. 把用例名加到 `run-ui.sh` 的 `SUITES` 数组。

约定：

- 不要用本机绝对路径，仓库文件从 `h.T` 下取；
- 排序类断言优先看请求参数 + 接口返回的顺序，其次看 DOM；
- 会改配置的用例要把旧值恢复，避免影响后面的用例。
