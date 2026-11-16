# rtunnel：A → B → C 远程 SSH 隧道

反向通道设计见 [`docs/design/rtunnel.md`](../docs/design/rtunnel.md)。

```
开发者 ssh ──本地 SOCKS5──> A ══TCP/TLS══> B ══TCP/TLS══> C ──> 127.0.0.1:22
```

| 程序 | 角色 |
|---|---|
| `rtunnel-relay` | B：接入、devid 注册表、ACL、c_sid 分配、帧转发 |
| `rtunnel-agent` | C：连 B、上报 devid、执行 OPEN、透传字节 |
| `rtunnel-client` | A：连 B、本地 SOCKS5 入口、把 `devid:port` 转成 OPEN |

## 文档

- 设计文档：`docs/design/rtunnel.md`
- 全部拓扑/流程/时序/状态图（Mermaid + ASCII）与协议/配置/状态/参数表：
  `docs/design/rtunnel-diagrams.md`

## 构建

```bash
./enp_build.sh --static --shared            # 产物在 build/bin/rtunnel-*
# 需要 TLS 时：
./enp_build.sh --static --shared --with-openssl
```

## 运行（明文）

```bash
# B（--client-users: 每行 user:token；--shell-users: 允许 OPEN_SHELL 的用户）
build/bin/rtunnel-relay --host 0.0.0.0 --port 8443 --acl alice='*' \
    --client-users /etc/rtunnel/clients.txt --shell-users alice
# C（设备侧，只允许本地 22 端口）
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22
# A（开发机，--token 是 relay 下发的客户端凭证）
build/bin/rtunnel-client --server B_IP --port 8443 --user alice --token <token> --socks5 127.0.0.1:1080
```

开发者 `~/.ssh/config`（SOCKS5 方式）：

```sshconfig
Host PILE-000123
    HostName PILE-000123
    User root
    ProxyCommand nc -X 5 -x 127.0.0.1:1080 %h %p
    HostKeyAlias %h
```

也可以走**本地端口映射**（不需要 SOCKS5 客户端）：

```bash
build/bin/rtunnel-client ... --forward 127.0.0.1:16022=PILE-000123:22
ssh -p 16022 root@127.0.0.1
```

```sshconfig
Host pile
    HostName 127.0.0.1
    Port 16022
    User root
    HostKeyAlias PILE-000123
```

## 运行（TLS）

```bash
build/bin/rtunnel-relay --port 8443 --acl alice='*' \
    --tls-cert server.crt --tls-key server.key
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22 \
    --tls-ca server.crt --tls-sni localhost
build/bin/rtunnel-client --server B_IP --port 8443 --user alice --socks5 127.0.0.1:1080 \
    --tls-ca server.crt --tls-sni localhost
```

## 多 relay 与故障切换

A/C 都支持多个 relay 地址，按顺序尝试，连上后保持（sticky，不自动切回）：

```bash
# 重复 --server 或逗号分隔都行；host[:port] 用 --port 作默认端口
build/bin/rtunnel-agent  --server r1.example.com:8443,r2.example.com:8443 --devid dev1 --allow 22
build/bin/rtunnel-client --server r1.example.com --server r2.example.com --port 8443 \
    --user alice --socks5 127.0.0.1:1080
# IPv6 用方括号：--server [2001:db8::1]:8443
```

- 最多 8 个地址；连接失败、握手/认证超时、已认证后掉线，都会切下一个地址
- 退避仍是 1s→30s，多地址时加 ±20% 抖动；AUTH_OK 后重置
- 任何认证失败（token 错/过期/禁用/超额、证书身份不匹配）都会把当前地址标记为不可用，
  继续试下一个；全部地址都被拒才停止重连（改完配置重启进程即可）
- client 的按需 agent 继承完整地址列表；client 切到新 relay 时会停掉旧 relay 上的随需 agent
- `shell <devid>` 子命令在建立会话前也会依次尝试后续地址

## 测试

```bash
make rtunnel_test                        # 明文
RTUNNEL_TLS=1 make rtunnel_test          # TLS（需要 --with-openssl 构建）
RTUNNEL_MTLS=1 make rtunnel_test         # mTLS：证书 SAN 必须等于 devid（另含负向用例）
RTUNNEL_UDS=1 make rtunnel_test          # 本地入口走 UDS（需要 -DENABLE_UDS=ON 构建）

python3 rtunnel/tests/e2e_failover.py    # 双 relay 故障切换
python3 rtunnel/tests/e2e_alerts.py      # 告警（本地 webhook sink）
build/bin/rtunnel-endpoint-test          # --server 地址解析单测
```

覆盖：CONNECT 建链、数据回环、离线设备、端口白名单、ACL、并发（两个 A 客户端同 sid 不串流）、
设备端关闭传播、OPEN 未决断开（CANCELLED）映射清理、256 KiB 大流量；另有 `build/bin/rtnp-codec-test` 校验帧编解码。

## mTLS 与 UDS

- mTLS：`--tls-ca` 让 B 要求客户端证书：C 侧校验证书 SAN 与 AUTH.devid 一致，
  A 侧校验证书 SAN 与 AUTH.user 一致（身份以证书为准，不接受自报）；
  C 侧用 `--tls-cert/--tls-key` 提供设备证书，`--tls-sni` 填 B 证书的主机名。
- UDS：`--socks5 unix:/path/to/socks.sock`，仅在使用 `-DENABLE_UDS=ON` 构建时可用
  （libhv 的 UDS 支持是编译期开关），默认仍是 `127.0.0.1:PORT`。

## 运维

- **ACL 热加载**：`--acl-file acl.txt`（每行 `user=devid[,devid|*]`，`#` 注释），改完
  `kill -HUP <pid>` 生效；只影响新 OPEN，不动已建立会话。整文件解析失败保留旧表。
- **优雅退出**：`SIGTERM`/`SIGINT` 停止 accept，等现有连接收尾（`--drain-timeout`，默认 10s）
  后退出；第二次信号立即强退。A/C 断线后按既有退避重连。
- **统计**：每 60s 一条 `[relay] stats:` 日志（agents/clients/streams/bytes_in/bytes_out）。

## 管理台（admin）

relay 启动加 `--admin 127.0.0.1:9443` 开启管理台（libhv HttpService，独立 worker 线程）：

- 能力：概览、设备/会话、踢会话/下线、权限（分组 / 有效权限 / 旧 ACL 导入导出）、接入账号与 Token（时效 / 流量 / 频率 / 并发）、证书签发/吊销、文件、审计、系统设置
- API 与权限矩阵：见 `docs/design/rtunnel.md` §13.3
- 文件分发：文件页「分发」→ 选择设备/时间窗/重试 → 每设备一条任务；
  `POST /api/v1/distributions` 创建，`GET /api/v1/distributions` 查询，
  任务页支持状态过滤、重试、取消、超时延长
- 文件哈希：上传即计算 sha256；同设备同哈希重复分发标记「冲突」，
  可校验设备文件（相同则直接成功）或强制重发
- 前端：构建 `rtunnel/relay/admin/web` 后，用 `--admin-docroot DIR` 指向 `dist/`；
  未指定时管理台页面不可用（仅 API 正常）
- 鉴权：`--admin-token T` 仅用于首次初始化管理员；之后使用账号密码登录
  （SQLite + HttpOnly 会话）；非回环监听必须配置 `--admin-token`；
- 证书配置：`--admin-ca-cert C --admin-ca-key K --admin-cert-dir D`（台账 index.txt、
  黑名单 revoked.txt、审计 audit.log 均落在 D 下，启动自动加载黑名单）；
- **HTTPS**：`--admin-tls-selfsigned` 自动生成并复用自签证书（EC P-256，
  SAN 覆盖 localhost、主机名、监听地址与本机全部 IP，目录 0700 / 私钥 0600，
  剩余不足 30 天自动重签）；也可以 `--admin-tls-cert C --admin-tls-key K` 用已有证书。
  启用后同一端口只开 HTTPS，明文 HTTP 不再监听；自签证书首次访问需手动信任。
  未加这两个参数时行为不变（明文 HTTP）。
- **告警**（设置页「告警」组）：总开关 + webhook，支持 `generic` / 企业微信 / 钉钉（加签）/
  飞书；事件为设备离线/恢复、证书到期（预警与紧急两级）、分发任务失败/超时；
  同类事件按冷却时间去重，`GET /api/v1/alerts/status` 看发送结果，
  `POST /api/v1/alerts/test`（或设置页「发送测试」）验证通道。webhook URL 与签名密钥
  对非 admin 脱敏，审计日志不记录明文密钥。
- 线程模型：HTTP 在 admin worker 线程，relay 状态访问经 `hloop_post_event` 投递回
  relay 线程执行（promise/future 同步），relay 数据无锁；分发/告警/清理等后台任务
  由 `AdminServer` 统一管理，`stop()` 时置标志并 join 全部 worker。

## 账号与权限（SQLite）

管理台账号存 SQLite（`--admin-db`，默认 `rtunnel-admin.db`），**需要 OpenSSL 构建**（PBKDF2 口令哈希）。

- **首次部署**：浏览器打开管理台 → 初始化页输入 `--admin-token` + 管理员用户名/密码 → 创建 admin；
  初始化前 token 只能访问 `/api/v1/setup*`
- **之后登录**：用户名 + 密码，服务端会话（HttpOnly Cookie，默认 12h，可在设置中调整），不再用 token
- **角色**（admin 唯一，其余可多个）：
  | 角色 | 权限 |
  |---|---|
  | admin | 全部权限；创建/切换 manager、operator、viewer |
  | manager | 绑定设备；创建 operator/viewer；切换其角色；设备与文件全部操作 |
  | operator | 设备 Shell / Web 代理；文件上传、下载；查看会话 |
  | viewer | 仅「概览」「设备列表」两个浏览页面 |
- **设备归属**：`PUT /api/v1/devices/{devid}/owner {"username":"alice"}`（空串=解绑）；
  operator/viewer 的设备与会话按归属过滤，未绑定设备仅 manager/admin 可见
- **账号 API**：`GET/POST /api/v1/users`、`PUT /api/v1/users/{id}/password|status`、
  `DELETE /api/v1/users/{id}`；`POST /api/v1/login|logout`、`GET /api/v1/me`
- 口令策略：默认最短 8 位、连续 5 次失败锁定 5 分钟（均可在「设置」调整）；改密/禁用会吊销全部会话

## 设备系统信息与能力

设备详情页（operator 及以上，viewer 可看自己名下设备）：

- 内存/CPU sparkline（可选「每 5s 刷新」，浏览器本地累积 60 点）
- 存储：按分区横向条，条长 = 分区总量 / 最大分区总量，条内按已用/空闲分色
- 网卡、系统（架构/内核/CPU/板卡/采集时间）、运行时（版本/能力/启动参数）
- 「能力」行展示 TCP 端口、Shell、SYSINFO、mTLS、OTA；manager/admin 可在线编辑

采集与配置：

- `SYSINFO_REQ(0x19)` / `SYSINFO(0x1A)`：按需采集，relay 仅内存缓存，不落库
- `CONFIG_SET(0x1B)` / `CONFIG_ACK(0x1C)`：在线修改端口与 Shell 能力，
  agent 原子生效并写入 `--config-file`（默认 `/opt/data/etc/agent.json`），重启后保持
- API：`GET /api/v1/agents/{devid}/sysinfo?refresh=1`、`PUT /api/v1/agents/{devid}/caps`
- 文件分发：文件页「分发」选择设备与时间窗，创建 `POST /api/v1/distributions`，
  `GET /api/v1/distributions` 查询进度；经 RTNP 推到设备 `--file-dir/dir`
  （默认 `/opt/data/files`），32KiB 分片、256KiB 窗口、sha256 校验、断点续传、
  冲突校验；`.part` 超过 24h 自动清理
  （端口 manager/admin；Shell 仅 admin；设备离线直接报错）

## Agent OTA 升级

OTA 不另建传输通道：升级包走「文件上传 + 文件分发」已实现的能力落到设备 `--file-dir`，
分发任务完成后由 relay 下发安装指令，按阶段推进并统一记录在 `distribution_tasks`：

1. 文件页上传新二进制（或直接放入 `--admin-file-dir`）；
2. `/ota` 选择升级包、目标版本（须与新二进制 `--version` 一致）、设备；
3. 传输阶段复用 `FILE_BEGIN/DATA/END`（0x1D-0x1F）：32KiB 分片、256KiB 窗口、
   sha256 校验、断点续传，任务状态 `running`；
4. relay 发 `OTA_APPLY`（0x30，JSON：path/version/size/sha256）：
   agent 校验文件大小与 sha256 → 执行 `--version` 自检（`verifying`）→
   校验通过后 ACK `verified`（`installing`）→ 复制到二进制目录、备份 `<self>.prev`、
   原子替换、ACK `installed` 并 `execv` 重启（`confirming`）；
5. agent 以新版本重连并上报 SYSINFO，relay 比对目标版本 → `success`。

任务状态：`scheduled→queued→running→verifying→installing→confirming→success`；
失败分支为 `failed / timeout / conflict / unsupported / canceled`，重试沿用分发任务的
退避与截止时间；重试会重新走传输（`.part` 续传）和安装。OTA 与普通分发任务都在
「任务」页查看，可按 `kind=ota` 过滤，任务详情显示目标版本和当前阶段。

设备侧前置条件与限制：

- agent 必须显式开启 `--allow-ota`（或 `agent.json` 的 `"ota": true`），默认关闭；
- 只对常驻 agent 生效，按需 agent（`a:<user>`）拒绝；
- 二进制所在目录必须可写；分发临时文件在 `--file-dir`；
- 同设备已有进行中的 OTA 时，新任务记 `conflict`；设备已是目标版本且未勾选
  「强制重装」也记 `conflict`；
- 升级会重启 agent，正在进行的隧道/shell 会断开；
- 失败时原二进制不变；替换后重启失败会自动回滚 `.prev`；
- 手动回滚：设备上 `mv rtunnel-agent.prev rtunnel-agent` 后重启；
- 版本确认超时 `ota.apply_timeout_s`（默认 180s）。

API：`POST /api/v1/distributions` 增加 `kind: "ota"` 与 `version`、`force` 字段；
查询复用 `GET /api/v1/distributions?kind=ota` 与 `.../summary`，重试/取消复用
`POST /api/v1/distributions/:id/retry|cancel`。

## Web 终端（免密）

「设备 C」「客户端 A」列表的 Shell 入口：admin 作为 RTNP client 直连本机 relay，
向目标 agent 发 `OPEN_SHELL`（flags=0x0001），agent `forkpty` 起交互 shell，
经 WebSocket 接到浏览器 xterm.js。不 fork ssh、不经手密码。
前端支持多 tab；切换页面只摘 DOM，不关闭 WebSocket 会话。

- 身份固定为 `--admin-shell-user`（凭证 `--admin-shell-token`），**不接受浏览器指定用户**；
- 设备列表：目标 devid = 设备 ID；客户端列表：目标 devid = `a:<user>`（按需拉起，见下节）；
- 需要 relay 的 `--shell-users` 允许该身份，且目标 agent 开 `--allow-shell`；
- 终端尺寸经 `RESIZE`(0x17) 帧同步。
A 端也可在本机执行 `rtunnel-client --server B --user U shell <devid>` 直连设备免密 shell。

## A 侧按需 Shell（client 列表的 Shell 按钮）

client 加 `--allow-shell`（默认关）后：

1. 管理台「客户端 A」点 Shell，admin 对 `a:<user>` 发 `OPEN_SHELL`；
2. relay 发现 `a:<user>` 未注册且该 user 的 client 在线，向其下发 `SPAWN_AGENT`，并挂起 OPEN（10s 超时）；
3. client 在本机 `fork/exec` 同目录的 `rtunnel-agent`（可用 `--agent-path` 指定），
   以 `a:<user>` 注册；relay 把挂起的 OPEN 继续转发给这个 agent；
4. 无会话空闲 `--agent-idle` 秒（默认 60）后 agent 自行退出，client 退出时也会回收。

client 侧只做「拉起/回收」，shell 逻辑完全复用 agent；relay 侧只多一个挂起状态和新控制帧，
不需要在 A-B 连接上叠双角色。

```bash
build/bin/rtunnel-client --server B --port 8443 --user alice --token <token> \
    --socks5 127.0.0.1:1080 --allow-shell --agent-idle 60
```

## 设备 Web 代理（路径式）

板子/设备上的 Web 服务可通过管理台直接访问，不需要 A 端或本地代理：

```
http://B:9443/device/<devid>/<port>/<path>
例：http://192.168.1.133:9443/device/board01/8080/
```

- **认证**：管理台 Cookie 会话；普通账号只能访问绑定到自己账号的设备
- **端口**：必须在 agent 的 `--allow` 列表里（如 `--allow 22,8080`）
- **数据面**：admin 作为 RTNP client 连本机 relay，`OPEN(devid, port)` 后转发
- **HTTP**：响应流式转发（chunked），不限制大小；`Location` 与 HTML/CSS 绝对路径做前缀改写
- **WebSocket**：`/device/<devid>/<port>/...` 同时支持 WS，帧级转发
- **Cookie**：设备 Cookie 加 `d_<devid>_<port>_` 前缀，与管理台会话隔离
- **限制**：同时最多 32 条代理请求（超出 503）；不支持设备侧 HTTPS；路径式对部分 SPA 仍可能缺资源；relay 需明文部署

设备列表的 **Web** 按钮会提示输入端口并新开窗口。

## 参数速查

**relay（B）**

| 参数 | 说明 |
|---|---|
| `--host/--port` | 隧道监听地址，默认 `0.0.0.0:8443` |
| `--acl user=dev1,dev2` / `--acl-file F` | 用户→设备 ACL；文件可 `SIGHUP` 热加载 |
| `--allow-all` | 关闭 ACL（仅调试） |
| `--client-users F` | 每行 `user:token`，强制 A 端凭证 |
| `--client-token T` | 全局客户端 token（`--client-users` 的简化版） |
| `--shell-users u1,u2` / `*` | 允许 `OPEN_SHELL` 的用户；默认空 = 全部拒绝 |
| `--tls-cert/--tls-key [--tls-ca]` | 服务端 TLS；给 `--tls-ca` 即要求客户端证书（mTLS） |
| `--admin ADDR --admin-token T` | 管理台监听；token 仅用于首次初始化 |
| `--admin-db PATH` | SQLite 账号库，默认 `rtunnel-admin.db`（需 OpenSSL 构建） |
| `--admin-shell-user U --admin-shell-token T` | Web 终端连本机 relay 的身份与凭证 |
| `--admin-ca-cert/--admin-ca-key/--admin-cert-dir` | 证书签发 CA 与台账目录 |
| `--admin-tls-cert/--admin-tls-key` | 管理台 HTTPS：使用已有证书 |
| `--admin-tls-selfsigned [--admin-tls-dir D] [--admin-tls-days N]` | 管理台 HTTPS：自签并自动续期 |
| `--admin-file-dir` / `--admin-docroot` | 文件管理目录 / 前端磁盘目录 |
| `--drain-timeout` | 优雅退出等待时长，默认 10000ms |

**client（A）**

| 参数 | 说明 |
|---|---|
| `--server/--port --user --token` | relay 地址与登录身份；`--server` 可重复或逗号分隔（最多 8 个，故障自动切换） |
| `--socks5 HOST:PORT` / `unix:PATH` | 本地 SOCKS5 入口；**可省略**，省略时只做 `--forward` / `shell`，不会再默认占用 1080 |
| `--forward 127.0.0.1:16022=devid:22` | 本地端口映射，可多次 |
| `--allow-shell` | 允许 B 通知本机按需拉起 agent（默认关） |
| `--agent-path` / `--agent-idle` | agent 路径 / 空闲自退秒数（默认 60） |
| `--tls-ca/--tls-cert/--tls-key/--tls-sni` | TLS / mTLS |
| `shell <devid>` | 本机终端直连设备免密 shell |

**agent（C）**

| 参数 | 说明 |
|---|---|
| `--server/--port --devid` | relay 地址与设备 ID；`--server` 可重复或逗号分隔（最多 8 个，故障自动切换） |
| `--allow 22,80` / `--allow-all` | 目标端口白名单 |
| `--allow-shell` / `--max-idle` | 允许 `OPEN_SHELL` / 空闲自退秒数 |
| `--allow-ota` | 允许 relay 通知安装 file_dir 中的新版本（默认关） |
| `--local-web [ADDR:]PORT` | 本地只读设备详情页（带 Web 构建），默认 `127.0.0.1:8080` |
| `--file-root DIR` | 文件管理根目录，默认同 `--file-dir`（`/opt/data/files`） |
| `--file-rw` / `--file-ro` | 允许/禁止写操作；二进制默认只读，部署服务默认开启读写 |
| `--file-max-mb N` | 单文件上传上限，默认 512 |
| `--file-token T` | 本地 Web 文件通道 token（relay-admin 代理路径由 relay 认证，不需要） |
| `--tls-ca/--tls-cert/--tls-key/--tls-sni` | TLS / mTLS |

### agent 本地设备详情页

带 `RTUNNEL_AGENT_WEB=ON`（默认）编译的 `rtunnel-agent` 内置设备详情页，
不依赖 relay 管理台；隧道断开时页面仍可打开（文件管理是否可写由 `--file-rw` 决定）：

```bash
./rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --local-web 0.0.0.0:8080
# 浏览器打开 http://<设备IP>:8080/
```

- `--local-web [ADDR:]PORT`：`8080` / `127.0.0.1:8080` 只监听本机；
  `:8080` / `0.0.0.0:8080` 监听所有网卡（页面会暴露设备型号、IP、串口等只读信息）
- 页面内容与管理台设备详情同源：连接状态、CPU/内存/负载曲线、存储、网卡、
  系统信息、能力配置、mTLS 证书、串口 TTY；每 2 秒刷新
- HTTP 只读接口：`GET /`、`GET /api/v1/sysinfo`、`GET /api/v1/health`；
  文件管理走 `GET /ws/fs`（WebSocket，见下节）
- 响应支持 gzip 协商（`Accept-Encoding: gzip`），HTML 与 JSON 都会压缩，并带 `Vary: Accept-Encoding`
- 数据由 agent 主循环采集并缓存，HTTP 线程只读快照，不直接访问隧道状态
- 两个构建变体：`rtunnel-agent`（带 Web）与 `rtunnel-agent-noweb`（纯隧道，体积更小）。
  `build/deploy/agent` / `build/deploy/agent-web` 是对应的两份部署目录，
  `deploy/package_agent.sh` 会打包为 `rtunnel-agent-lite-*.tar.gz` 与
  `rtunnel-agent-web-*.tar.gz`（解压后都是 `agent/`，可直接替换）

### 文件管理（WebSocket）

设备详情页的「文件管理」卡片提供浏览、上传、下载、新建/重命名/移动/删除、
文本编辑和图片预览。本地页面与管理台共用同一个 WebSocket 协议和同一份前端
（`shared/web/fs-ui.js`），relay 只转发数据流，不解析文件协议：

```
本地页面  ws://device:8080/ws/fs
管理台    wss://relay/ws/fs/<devid>
              |
              v
        RTNP OPEN_FS + DATA(u8 kind + ws message)
              |
              v
        agent FsSession -> --file-root
```

- 协议：文本帧为 JSON 控制消息（`hello/list/download_open/upload_open/...`），
  二进制帧为 16 字节头（`id/kind/offset`）+ 数据；单帧 ≤ 30 KiB
- 上传分片写入目标目录的 `.rtunnel-upload/<id>.part`，完成后 `fsync + rename` 原子落盘；
  页面刷新或断线后可用 `upload_id` 从已接收位置续传
- 下载按 30 KiB 分块，客户端逐块 `download_ack` 流控；文件夹下载走 `archive_open`，
  由 agent 边遍历边打 tar + gzip 流式输出（保留符号链接、空目录和长文件名），
  前端优先用 File System Access API 边收边写盘
- 路径栏为可点击面包屑，可直接跳到任意上级目录；「编辑」可切换为手动输入路径
- 双击文件节点打开：文本进编辑器、图片进预览、其他类型直接下载；操作列只保留
  下载/重命名/移动/删除，按可用宽度平铺，放不下的收进 `⋯` 菜单；
  信息行显示根目录、单文件上限和分区已用/总容量
- 重命名和移动在列表内展开表单（回车确认、Esc 取消），不弹浏览器对话框
- 文本编辑使用 CodeMirror 6（shell/json 语法高亮，Ctrl+S 保存、Esc 关闭），
  组件打包为 `shared/web/cm-editor.bundle.js`：管理台经 Vite 作为带 hash 的静态资源引入，
  本地页由 CMake 内嵌到二进制并从 `/assets/cm-editor.js` 加载；未构建时自动退化为纯 textarea
- JSON 文件在编辑器里点「树形」切换为树形查看：展开/收起、按需渲染（大文件不会卡）、
  解析失败给出提示；树形里点「源码」可切回编辑器，未保存内容一并带过去
- 路径隔离：`--file-root` 为唯一根，拒绝 `..`、绝对路径和逃出根目录的符号链接
  （Linux 5.6+ 用 `openat2(RESOLVE_BENEATH)`，MP135 5.15 可用）
- 管理台角色：`operator` 只读，`manager/admin` 读写；agent 侧还会再按 `--file-ro/--file-rw`
  和 `--file-token` 校验一次
- 管理台「能力配置」可启停文件服务并修改根目录、读写模式、单文件上限，
  热生效并写入 agent 配置文件（`--config-file`，默认 `/opt/data/etc/agent.json`）；
  旧版 agent 不认识这些字段，需要先升级
- relay-admin 通过本机 relay 打开管理流，需要配置 `--admin-shell-user/--admin-shell-token`
  并确保该用户对目标设备有 ACL（与 Web 终端一致）
- 本地 Web 与 relay 两条路径都可单独使用：lite 包没有本地页面，但管理台仍能管理文件

```bash
# 本地页面 + 局域网可写
./rtunnel-agent --devid board01 --server 192.168.1.133 --port 18443 \
    --file-root /opt/data/files --file-rw \
    --local-web 0.0.0.0:8080 --file-token <token>
```

## 三机部署示例

```bash
# B（192.168.1.133）：隧道 + 管理台
printf 'dev:tok-dev\nops:tok-ops\n' > /tmp/rtunnel/clients.txt
./rtunnel-relay --host 0.0.0.0 --port 18443 \
    --acl ops='*' --client-users /tmp/rtunnel/clients.txt --shell-users ops \
    --admin 0.0.0.0:9443 --admin-token <admin-token> \
    --admin-ca-cert ca.crt --admin-ca-key ca.key --admin-cert-dir certs \
    --admin-file-dir files --admin-shell-user ops --admin-shell-token tok-ops

# C（192.168.1.55，板子）
./rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --allow-shell

# A（开发机）
./rtunnel-client --server 192.168.1.133 --port 18443 --user dev --token tok-dev \
    --socks5 127.0.0.1:11080 --forward 127.0.0.1:16022=board01:22 \
    --allow-shell --agent-idle 120
```

```bash
ssh -p 16022 root@127.0.0.1                         # 到板子
# 管理台：http://B:9443/ ，初始化 admin 后登录；客户端列表 Shell = 本机 shell
```

## 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| Web 终端显示「连接已关闭」 | 1.3.19 之前 `shell_relay_port` 不跟随 `--port`；升级 relay 或让 relay 监听 8443 |
| 客户端列表 Shell 没反应 | client 未加 `--allow-shell`，或该用户不在 `--shell-users`，或 devid 不是 `a:<user>` |
| A 端上线即断开 | relay 配了 `--client-users/--client-token`，client 缺 `--token` 或 token 不匹配 |
| `OPEN_FAIL: shell not allowed` | relay 的 `--shell-users` 未包含该用户 |
| `OPEN_FAIL: shell disabled` | agent 未加 `--allow-shell` |
| client 启动报 `bind: Address already in use` | 默认会监听 1080，换 `--socks5 127.0.0.1:<port>` |
| 部署后 `undefined symbol: hv_version` | 用了共享库构建只拷了二进制；用 `--no-shared` 静态构建或一并部署 `libhv.so` |

## 实现说明

- **客户端认证**：relay 配了 `--client-users`（user:token）或 `--client-token` 后，
  A 端必须带 `--token` 才能上线；未配置时启动日志会打印 warning（开发模式）。
- **Shell 授权**：`OPEN_SHELL` 需要 relay 的 `--shell-users` 且设备 agent 开 `--allow-shell`，
  两者缺一即 `OPEN_FAIL`。管理台的 Web 终端固定用 `--admin-shell-user` 身份
  （`--admin-shell-token` 提供该身份连 relay 的凭证），不再接受浏览器指定用户。
- **不改动 libhv**：所有能力都在 `rtunnel/` 内实现。
- **目录结构（C++ 对象化）**：`rtnp/` 协议库（Frame/Decoder/Writer/Reader）、`common/` 共享组件
  （`TunnelLink`：agent/client 长连接的 HELLO/AUTH 状态机、心跳判死、超时、退避重连）、
  `client/`、`agent/`、`relay/` 各自目录；状态收进 App/Server 类成员，
  libhv 回调经 `hio_context`/`hevent_userdata` 取回对象，Buffer/Writer 等缓冲区 RAII 管理。
- mTLS 的 SAN 提取在 `relay/relay_server.cpp` 内直接用 OpenSSL 完成（`hio_get_ssl()` 是公开 API），
  仅 `WITH_OPENSSL` 构建时编译。
- 流控按设计文档：发送方受 credit 限制；接收方在本地写队列降到 `RTNP_LOW_WATER` 后才回 `WINDOW`。
- 超时体系：HELLO 5s / AUTH 10s / 未认证连接 10s；OPEN 15s；半关闭 60s 未对关发
  `RESET(CLOSE_TIMEOUT)`；心跳 15s、连续 3 个 PING 无 PONG 判死重连。
- **限制**：libhv 在本地 socket 读到 EOF 时会关闭 io，因此「SOCKS 客户端先 FIN、
  再由 A 回写剩余数据」不支持；SSH 正常关闭（设备端先关）不受影响。

