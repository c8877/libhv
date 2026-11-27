# agent（C）

设备侧常驻进程。只做出站连接（能过 NAT），连上 relay 后上报 `devid`，接受 `OPEN`
并透传字节；另外负责采集系统信息、执行受控命令、管理文件与服务，可选带一个本地设备页。

## 运行

```bash
build/bin/rtunnel-agent --server 192.168.1.133 --port 8443 --devid board01 --allow 22

# 带本地页（局域网可访问）
build/bin/rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --local-web 0.0.0.0:8080 --file-rw --file-token <token>
```

## 参数

| 参数 | 说明 |
|---|---|
| `--server/--port --devid` | relay 地址与设备 ID；`--server` 可重复或逗号分隔（最多 8 个） |
| `--allow 22,80` / `--allow-all` | 目标端口白名单（不配则不能转发） |
| `--allow-shell` / `--max-idle` | 允许 `OPEN_SHELL` / 空闲自退秒数 |
| `--allow-exec "cmd a *;cmd b"` | 受控执行白名单（分号分隔的 argv 模板，逐段通配）；不配则 `EXEC` 不可用 |
| `--exec-timeout` / `--exec-max-bytes` | 单次执行超时（默认 30000ms）与输出上限（默认 256 KiB） |
| `--allow-ota` | 允许 relay 通知安装新版本（命令行默认关；部署包自带配置为开） |
| `--local-web [ADDR:]PORT` | 本地设备页，默认 `127.0.0.1:8080` |
| `--file-root DIR` | 文件管理根目录，默认同 `--file-dir`（`/opt/data/files`） |
| `--file-rw` / `--file-ro` | 允许/禁止写操作；二进制默认只读，部署服务默认开读写 |
| `--file-max-mb N` | 单文件上传上限，默认 512 |
| `--file-token T` | 本地页写操作令牌（relay 代理路径由 relay 认证，不需要） |
| `--svc-rw` / `--svc-token` / `--no-svc` | 服务管理写权限 / 令牌 / 完全关闭 |
| `--tls-ca/--tls-cert/--tls-key/--tls-sni` | TLS / mTLS |

## 配置落盘

能力配置加载顺序：`--config-file`（默认 `/opt/data/etc/rtunnel-agent.json`）→ 文件不存在时
用命令行参数；文件中未出现的字段沿用命令行值，文件值优先。首次在线修改能力或采样配置时
创建该文件。

```json
{
  "devid": "board01",
  "relay_servers": ["r1.example.com:8443", "r2.example.com:8443"],
  "relay_servers_rev": 7,
  "allow": [22, 8080],
  "allow_shell": true,
  "allow_exec": ["systemctl restart {svc}"],
  "ota": true,
  "file_root": "/opt/data/files",
  "file_rw": true,
  "local_web": "127.0.0.1:8080",
  "svc_rw": false,
  "metrics": { "cpu": {"enabled": true}, "proc:1234:cpu": {"enabled": true} }
}
```

身份参数（`AGENT_ARGS`）在 `/opt/data/etc/rtunnel-agent.args`，unit 只读这一行：

```
AGENT_ARGS=--devid board01 --server 10.0.0.10:18443 --allow 22
```

设备侧可变文件统一放 `/opt/data`：能力配置、身份参数、文件服务目录 `/opt/data/files`。
这是数据分区，重启/升级后保留。

## 系统信息与受控能力

relay 通过 `SYSINFO_REQ(0x19)` / `SYSINFO(0x1A)` 按需采集（relay 只内存缓存，不落库）；
`CONFIG_SET(0x1B)` / `CONFIG_ACK(0x1C)` 在线改能力，agent 原子生效并写配置文件。

- `GET /api/v1/agents/{devid}/sysinfo?mode=full|inc`：CPU/内存/负载/网卡/存储/温度/eMMC 寿命/
  系统信息/证书状态。挂载百分比按 已用/总量；同一文件系统 bind mount 只报最短路径；
  离线或回帧超时返回带 `cached:true` 的缓存快照，后台线程等回帧，不阻塞其它请求。
- `GET /api/v1/agents/{devid}/net?scope=tcp|udp|unix|ipc|all&proc=1&page=&page_size=`：
  读 `/proc/net/*` 与 `lsipc`（缺失回退 `/proc/sysvipc`；额外扫 `/dev/shm` 上报 POSIX
  共享内存）。过滤与分页在 agent 完成，relay 缓存 3s，`refresh=1` 绕过。
- `GET /api/v1/agents/{devid}/procs?sort=&q=&page=&page_size=`、`.../procs/{pid}`、
  `POST .../procs/signal {pid,signal}`：扫 `/proc/<pid>/*`，CPU 首次 300ms 双采样，
  之后用累计 CPU 时间差。PID 1 与 agent 自身拒绝发信号。查看要 `device.proc.read`，
  信号要 `device.proc.signal`。
- `GET /api/v1/agents/{devid}/storage?mount=/&path=&depth=`：只回 `path` 下 `depth` 层内节点，
  展开再按路径请求。node = `{p,b,n,d,m,t,c}`（路径/字节/文件数/子目录数/修改时间/类型/还有子项）；
  `t` 为 `d`/`f`/`o`（小文件聚合）。文件只回 ≥64KB 的，其余按目录聚合，保证子项之和等于父项。
  同一 `mount/path/depth` 结果 60s 复用（`force=1` 重扫）；单次上限 20 万目录项 / 20s，
  跳过无权限目录与其它分区，只读元数据，权限复用 `device.view`。
- `GET /api/v1/agents/{devid}/diag`：返回 `application/gzip` 诊断包（sysinfo、dmesg、
  journal 500 行、`ip addr/route`、`ss -s`、服务列表、前 30 进程、网络快照、本机配置），
  上限 8MB，权限 `device.diag`。
- `POST /api/v1/agents/{devid}/power {action,delay_ms,password}`：必须带设备 root 密码，
  agent 用 `/etc/shadow` 哈希校验，连续 5 次失败冷却 5 分钟；ACK 先回，到点再执行。
  `reboot` / `poweroff` 是两个独立开关（能力页分别开启），资源 `device.power`，默认关闭。
- 受控执行：`POST /api/v1/agents/{devid}/exec` 执行 `--allow-exec` 白名单里的 argv
  （不经 shell），stdout/stderr 分流、超时强杀、超字节截断，写审计 `device.exec`。

## 本地设备页

带 `RTUNNEL_AGENT_WEB=ON`（默认）编译的 agent 内置设备页，不依赖 relay 管理台；
隧道断开时页面仍能打开（能否写由 `--file-rw` 决定）。

```bash
./rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --local-web 0.0.0.0:8080
```

- `--local-web`：`8080` / `127.0.0.1:8080` 只监听本机；`:8080` / `0.0.0.0:8080` 监听所有网卡。
  页面包含文件、服务等**可写**操作，同源检查只防浏览器跨站，**不能代替鉴权**：
  对外监听时必须同时配 `--file-token` / `--svc-token`，否则同网段可直接读写文件、控制系统服务。
- 页面与管理台设备详情是同一份 Vue 源码（`components/DeviceView.vue`），本地入口在
  `src/local`，通过 API 适配直连 agent；`npm run build:local` 打成单个 HTML 写入
  `agent/web/index.html`，由 CMake 内嵌，不加载外部资源。
- 单层 tab（概览/存储/文件/服务/进程/网络/监控/串口/能力）；本地页没有 relay 会话，
  概览不显示「会话」卡，能力/采样/串口配置只读。非活动 tab 不请求，首次进入才加载。
- 本地页与 relay 详情共用预热：agent 启动和页面打开时刷新进程/服务缓存，页面打开期间由
  2s 的 sysinfo 轮询续期（关闭后 90s 停止），服务列表不用等 `systemctl` 首次全量扫描
  （单核设备约 4s）。
- HTTP：`GET /`、`/api/v1/sysinfo`、`/api/v1/procs`、`/api/v1/net`、`/api/v1/metrics`、
  `/api/v1/storage/tree`、`/api/v1/health`；文件与服务走 `GET /ws/fs`、`GET /ws/svc`。
- 可能阻塞的接口（进程/网络/存储/诊断）用异步 handler 交给 `hv::async`，采集在共享 worker
  串行执行，不占 HTTP IO 线程。
- 页面与 `fs-ui.js`/`svc-ui.js`/`storage-ui.js`/`file-view.js`/`cm-editor.js` 构建时压成 gzip
  以字节数组嵌入，运行时直接发压缩体；响应支持 `Accept-Encoding` 协商并带
  `Vary: Accept-Encoding`。
- sysinfo 快照在 loop 线程生成：页面打开（15s 内有请求）时 2s 一次，无人访问降到 10s 一次；
  页面请求时会立即补一次，首屏不等定时器。
- 两个构建变体：`rtunnel-agent`（带 Web）与 `rtunnel-agent-noweb`（纯隧道，体积更小）。
  体积敏感选 lite 包，**不要为了减体积再写一套精简页面**。两者共用一个
  `rtunnel-agent.service`，变体只由 `bin/rtunnel-agent` 软链指向哪个版本决定。
  `"local_web": false` 可关闭本地页，关闭后不预热，空闲 RSS 与 noweb 基本一致。

### 线程与内存

线程只在用到对应功能时创建，不随连接数增长。`ps -L` 或 `/proc/<pid>/task/*/comm` 可看到名字：

| 线程 | 创建时机 | 作用 |
|---|---|---|
| 主事件循环 | 启动 | 隧道帧、定时器、所有 agent 状态 |
| 查询 worker（`rtagent-query`） | 首次进程/网络/诊断请求；web 版本地页预热 | 进程双采样、网络/IPC 快照、诊断包 |
| 阻塞 worker（`rtagent-work`） | 首次服务列表或展开存储分区 | systemctl/journalctl 与目录树扫描共用一个线程，服务优先；存储结果 60s 复用 |
| HTTP IO | 启用 `--local-web` | 本地页与 WebSocket |
| `hv::async` | 首个异步接口请求 | 进程/网络/存储接口，上限 4、空闲 15s 退出 |

纯隧道且没人打开详情页时只有主线程；打开页面后按用到的功能增加，停止轮询 15s 后退出。
扫描不并进查询 worker 是有意的：一次分区遍历上限 20s，`systemctl restart` 也可能几秒，
而进程/网络页每 2s 刷新，共用一条队列会互相等。

RSS 里约 5–6 MB 是可回收文件页（二进制、libc/libstdc++/libcrypto/libssl），匿名内存主要来自
本地页的进程预热缓存。

## 文件管理

设备详情页的「文件」tab 提供浏览、上传、下载、新建/重命名/移动/删除、文本编辑、图片预览。
本地页与管理台共用同一个 WebSocket 协议和同一份前端（`rtunnel/shared/web/fs-ui.js`），relay 只转发，
不解析文件协议：

```
本地页面  ws://device:8080/ws/fs
管理台    wss://relay/ws/fs/<devid>
              → RTNP OPEN_FS + DATA(u8 kind + ws message) → agent FsSession → --file-root
```

- 文本帧是 JSON 控制消息（`hello/list/download_open/upload_open/...`），二进制帧是
  16 字节头（`id/kind/offset`）+ 数据；单帧 ≤30 KiB。
- 上传分片写 `.rtunnel-upload/<id>.part`，完成 `fsync + rename` 原子落盘；刷新/断线后可用
  `upload_id` 续传。
- 下载按 30 KiB 分块、客户端逐块 `download_ack` 流控；文件夹走 `archive_open`，
  agent 边遍历边打 tar+gzip 流式输出（保留符号链接、空目录、长文件名）。
- 路径栏是可点击面包屑，可切手动输入；双击文件行展开详情卡片，双击目录进入目录。
- 重命名/移动在列表内展开表单（回车确认、Esc 取消），不弹浏览器对话框。
- 文本编辑用 CodeMirror 6（shell/json 高亮，Ctrl+S 保存、Esc 关闭）；JSON 可切树形查看。
- 路径隔离：`--file-root` 是唯一根，拒绝 `..`、绝对路径和逃出根的符号链接
  （Linux 5.6+ 用 `openat2(RESOLVE_BENEATH)`）。
- 权限：管理台 `file.read` / `file.write`；agent 侧再按 `--file-ro/--file-rw` 与
  `--file-token` 校验一次。
- 管理台「能力配置」可启停文件服务并改根目录、读写模式、单文件上限，热生效并落盘。

## 服务管理

「服务」tab 读 systemd 服务列表，支持启动/停止/重启/启用/禁用/屏蔽，并查看最近 200 行
journal。本地页与管理台共用 `rtunnel/shared/web/svc-ui.js`：

```
本地页面  ws://device:8080/ws/svc
管理台    wss://relay/ws/svc/<devid>
              → RTNP OPEN_SVC + DATA(u8 kind=0 + JSON) → agent Service → systemctl/journalctl
```

- 列表先 `systemctl show '*.service'` 拿已加载单元，再合并 `list-unit-files`，
  保证停止且未启用的单元也能列出（否则无法从界面重新启用）。
  排序：失败 → 启动/停止中 → 运行中 → 已停止。
- 状态行显示 PID 与运行时长（`ActiveEnterTimestampMonotonic` + `/proc/uptime`，不受时钟跳变影响）；
  单核设备 `systemctl show` 约 1 秒，列表缓存 10 秒，动作后立即失效。
- 分页与排序在 agent 完成，只回当前页（默认 10，可选 5/10/20/50）；搜索按名称/描述，
  「全部/运行中/失败/已停止」带计数；列表 5s 刷新，展开日志 2s，页面不可见时暂停。
- 停/重启/禁用/屏蔽在行内展开确认条，不弹对话框；用 `systemctl --no-block` 排队。
- 所有 `systemctl`/`journalctl` 在独立工作线程执行，不阻塞隧道与本地 Web 循环；
  journal 输出限 256 KiB。
- 默认只读：`--svc-rw`（或配置 `svc_rw: true`）才允许控制；`--no-svc` 完全关闭（不建线程）。
  非 systemd 设备（无 `/run/systemd/system`）自动隐藏入口并上报 `caps.svc = false`。
- 管理台权限：`device.svc`；实际能否写仍由 agent 的 `--svc-rw` 决定。

## 共享前端

文件、服务、存储三块交互在 `rtunnel/shared/web/`，由两边各自挂载：

| 组件 | 作用 | 设备本地页 | 管理台 |
|---|---|---|---|
| `fs-ui.js` | 文件管理（WS 会话、上传下载、编辑器） | 打包进本地页 | `FileManagerCard.vue` 异步引入 |
| `svc-ui.js` | 服务管理（systemd 列表/日志/启停） | 打包进本地页 | `SvcManagerCard.vue` 异步引入 |
| `storage-ui.js` | 存储分区折叠、目录瀑布、右键递归展开 | 打包进本地页 | `StorageTreeCard.vue` 异步引入 |
| `file-view.js` | Hex / 文本 / 图片查看器 | 打包进本地页 | 与 `fs-ui` 共用 |

都用 `createXxx(opts)` 挂到传入容器，样式自带作用域（`fs-ui` 用 Shadow DOM，
`storage-ui` 规则挂在 `.rtstore` 下），不依赖页面框架，也不污染宿主页面。

## 升级

OTA 流程与回滚见 [`ota.md`](ota.md)。
