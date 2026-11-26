# rtunnel：A → B → C 远程 SSH 隧道

反向通道设计见 [`docs/design/rtunnel.md`](../docs/design/rtunnel.md)。

```
开发者 ssh ──本地 SOCKS5──> A ══TCP/TLS══> B ══TCP/TLS══> C ──> 127.0.0.1:22
```

| 程序 | 角色 |
|---|---|
| `rtunnel-relay` | B：接入、devid 注册表、权限组、c_sid 分配、帧转发 |
| `rtunnel-agent` | C：连 B、上报 devid、执行 OPEN、透传字节 |
| `rtunnel-client` | A：连 B、本地 SOCKS5 入口、把 `devid:port` 转成 OPEN |

## 文档

- 设计文档：`../docs/design/rtunnel.md`
- 全部拓扑/流程/时序/状态图（Mermaid + ASCII）与协议/配置/状态/参数表：
  `../docs/design/rtunnel-diagrams.md`
- 接入账号与 Token：`../docs/design/account-token.md`；账号详情页：`../docs/design/account-detail.md`
- 部署（测试环境直推、验证、回滚）：`deploy/DEPLOY.md`

## 构建

```bash
# 在仓库根目录（libhv/）执行；产物在 build/bin/rtunnel-*
./enp_build.sh --static --shared
# 需要 TLS 时：
./enp_build.sh --static --shared --with-openssl
```

## 运行（明文）

```bash
# B：隧道 + 管理台（权限、账号、Token 全部在管理台维护）
build/bin/rtunnel-relay --host 0.0.0.0 --port 8443 \
    --admin 0.0.0.0:9443 --admin-token <首次初始化令牌> --admin-db /var/lib/rtunnel/admin.db
# C（设备侧，只允许本地 22 端口）
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22
# A（开发机）：只有 rtk- 令牌，不需要 --user
build/bin/rtunnel-client --server B_IP --port 8443 --token rtk-... --socks5 127.0.0.1:1080
```

第一次部署：浏览器打开 `http://B:9443/`，用 `--admin-token` 初始化管理员，然后在
「权限 → 接入账号」建账号、签发 `rtk-` 令牌，并在「权限 → 权限组」里授权设备范围
（未分组 = 无权限）。要给不同客户分开计费/隔离设备时，先在「监控 → 租户」建租户，
把设备划到租户、账号也选同一租户；令牌只能访问本租户设备，用量按租户归集。

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
build/bin/rtunnel-relay --port 8443 \
    --tls-cert server.crt --tls-key server.key
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22 \
    --tls-ca server.crt --tls-sni localhost
build/bin/rtunnel-client --server B_IP --port 8443 --token rtk-... --socks5 127.0.0.1:1080 \
    --tls-ca server.crt --tls-sni localhost
```

## 多 relay 与故障切换

A/C 都支持多个 relay 地址，按顺序尝试，连上后保持（sticky，不自动切回）：

```bash
# 重复 --server 或逗号分隔都行；host[:port] 用 --port 作默认端口
build/bin/rtunnel-agent  --server r1.example.com:8443,r2.example.com:8443 --devid dev1 --allow 22
build/bin/rtunnel-client --server r1.example.com --server r2.example.com --port 8443 \
    --token rtk-... --socks5 127.0.0.1:1080
# IPv6 用方括号：--server [2001:db8::1]:8443
```

- 最多 8 个地址，**顺序即优先级（第 0 个最高）**：首次连接和每次重连都从高到低尝试，
  已建立的连接不会被更高优先级的地址抢占
- 连不上/握手超时的地址进入 30s 冷却，冷却期内跳过；没有可用地址时挑冷却最早到期的探测
- 退避仍是 1s→30s，多地址时加 ±20% 抖动；AUTH_OK 后重置
- 任何认证失败（token 错/过期/禁用/超额、证书身份不匹配）都会把当前地址标记为不可用，
  继续试下一个；全部地址都被拒才停止重连（改完配置重启进程即可）
- client 的按需 agent 继承完整地址列表；client 切到新 relay 时会停掉旧 relay 上的随需 agent
- `shell <devid>` 子命令在建立会话前也会依次尝试后续地址

### 由 relay 下发地址列表

设备上线后，relay 可以把最新的地址列表同步到 agent，agent 写入本地配置文件
`rtunnel-agent.json`（`relay_servers` / `relay_servers_rev`），重启后继续使用。

管理台入口在「运维 → 中继列表」，地址维护和下发是两件事：

- **中继地址** tab 维护地址字典（host/port、名称、备注、启停），勾选后「加入下发列表」，
  用 ↑/↓ 调整优先级（最多 8 个）。「保存列表」只更新期望状态
- **设备** tab 勾选目标设备，按需设置开始时间、一次性/分批灰度、每批台数、批间隔、
  失败阈值、截止时间和最大重试，创建同步任务
- 任务逐台下发并等待 ACK：成功/失败/待上线分别记录；中间批次失败率超过阈值会暂停，
  可继续、取消或重试（最后一批直接收敛成最终状态，不再走闸门）
- 任务创建时快照列表和 rev，之后改列表不会改写已建任务；保存新列表会暂停快照过期的任务，
  恢复时要求任务 rev 与当前列表一致，否则只能取消后重新创建
- 没有失败但仍有设备待上线的任务记为「部分完成」，不会当成成功
- 中继同步任务只在中继列表页展示，不进入「运维 → 任务」中心的文件/OTA 任务列表
- 每次 agent 认证成功，relay 自动下发当前列表，新设备/重启设备自动对齐
- `--server` 里的地址不会被删除：启动时放在下发列表末尾做兜底，列表写错也能救回设备
- 新列表不含 agent 当前连接的 relay 时默认拒绝；确认要迁移再打开「允许迁移」
- 接口：`/api/v1/relay-dict`（地址字典 CRUD/批量导入）、`/api/v1/relay-list`
  （有序列表 + 各设备 ACK）、`/api/v1/relay-sync/tasks`（创建/详情/暂停/继续/取消/重试），
  权限 `relay.read` / `relay.write`

### 管理边界：多 relay 只做链路冗余，不是管理高可用

每个 relay 都是**独立管理域**。`--admin-db` 指向本机 SQLite，账号、权限组、API Token、
接入账号、审计、证书台账、文件仓库、分发/OTA 任务、采样配置各存一份，互不同步：

| 数据 | 多 relay 之间 |
|---|---|
| 设备在线状态 / 隧道 | 不共享；设备同一时刻只连一个 relay |
| 管理台账号、会话、权限组 | 不共享；换 relay 要重新登录，且账号可能不存在 |
| API Token、接入账号 Token | 不共享；r1 签发的 token 在 r2 无效 |
| 审计、证书台账、分发/OTA 任务 | 不共享 |
| agent 本地采样配置 | 跟设备走；但 relay 侧的任务和权限记录不跟 |

因此：

- **设备切到 r2 后，r1 管理台看不到它**，r1 上的会话/统计也不再包含它；这是预期行为，
  不是“设备丢了”。排查时先确认设备当前连的是哪个 relay（relay 日志或 `GET /api/v1/agents`）。
- **不要把两个 relay 指向同一个 SQLite 文件**（包括 NFS/共享盘）。SQLite 的锁不跨主机，
  并发写会损坏库；当前也不支持共享库、主从或集群模式。
- **负载均衡不要把管理请求轮询到多个 relay**。管理台只应连一个固定 relay；LB 只用于
  数据端口且需要源地址保持，否则设备和管理台会落到不同实例。
- 需要“一个入口管所有设备”时，做法是**只保留一个 relay 承担管理**，其余 relay 作为
  纯链路备份（不暴露管理台、不存业务数据）；或后续引入集中管理库（当前未实现）。
- 多 relay 部署时，每个 relay 的 `--admin-token`、CA、TLS 证书、文件仓库都要分别初始化；
  设备侧 `--server` 列表可以共用同一份配置。

## 测试

```bash
make rtunnel_test                        # 明文
RTUNNEL_TLS=1 make rtunnel_test          # TLS（需要 --with-openssl 构建）
RTUNNEL_MTLS=1 make rtunnel_test         # mTLS：证书 SAN 必须等于 devid（另含负向用例）
RTUNNEL_UDS=1 make rtunnel_test          # 本地入口走 UDS（需要 -DENABLE_UDS=ON 构建）

python3 rtunnel/tests/e2e_failover.py    # 双 relay 故障切换
python3 rtunnel/tests/e2e_alerts.py      # 告警（本地 webhook sink）
bash rtunnel/tests/ota_e2e.sh            # OTA 安装/重启/版本确认 + 错误架构拒绝
bash rtunnel/tests/dist_scope_e2e.sh     # 分发/OTA 的设备范围授权
bash rtunnel/tests/sysinfo_e2e.sh        # 挂载字段全量/增量 + 离线缓存 + 管理台不阻塞
bash rtunnel/tests/net_e2e.sh            # 网络/IPC 快照、进程归属、缓存与离线回退
bash rtunnel/tests/proc_e2e.sh           # 进程列表/详情/信号与 device.proc 权限
bash rtunnel/tests/storage_e2e.sh        # 目录树按路径/层级、子项求和、越界校验与 gzip 透传
python3 rtunnel/tests/stream_idle_e2e.py # 流空闲回收：闲置 RESET、活跃流保留
python3 rtunnel/tests/permission_e2e.py  # 权限：混合组的端口范围、WS 消息按会话判定身份
python3 rtunnel/tests/usage_e2e.py       # 用量：流量/会话/设备在线计量、日聚合、权限
python3 rtunnel/tests/tenant_e2e.py      # 多租户：隔离、停用、用量归集、套餐（设备/并发/流量）
python3 rtunnel/tests/bandwidth_e2e.py   # 带宽整形：限速生效、数据不丢、不限速不受影响
python3 rtunnel/tests/billing_e2e.py     # 费用：价目、金额、按租户汇总、结算单状态机与导出
python3 rtunnel/tests/admin_embedded_e2e.py  # 内嵌前端：无 docroot 出页面、gzip、回退与优先级
python3 rtunnel/tests/packages_e2e.py    # 部署包：打包→发布→client 自举安装→篡改拒绝→回滚
python3 rtunnel/tests/exec_e2e.py        # 受控执行：白名单、分流、超时截断、作业 plan/apply/verify
python3 rtunnel/tests/events_e2e.py      # 事件流 SSE 与分钟级指标：上下线/会话/审计/续传/隔离
python3 rtunnel/tests/playbook_e2e.py    # 自愈规则：自动执行/超限转审批/冷却/总开关/越权
python3 rtunnel/tests/ai_e2e.py          # AI 助手：配置、测试连接、问答、提议作业/规则、越权、上游报错
python3 rtunnel/tests/mcp_e2e.py         # OpenAPI 与 MCP：工具清单、只读默认、前缀过滤、审计标记
python3 rtunnel/tests/compat_e2e.py      # HELLO 版本区间、低版本拒绝、高版本向前兼容
python3 rtunnel/tests/protocol_doc_test.py  # 协议文档覆盖率（帧类型/常量）
build/bin/rtunnel-endpoint-test          # --server 地址解析单测

rtunnel/tests/ui/run-ui.sh               # 管理台 + 设备本地页 UI 全量（Playwright）
rtunnel/tests/ui/run-ui.sh admin/smoke   # 只跑一个用例，list 可列全部
```

覆盖：CONNECT 建链、数据回环、离线设备、端口白名单、权限组、并发（两个 A 客户端同 sid 不串流）、
设备端关闭传播、OPEN 未决断开（CANCELLED）映射清理、256 KiB 大流量；另有 `build/bin/rtnp-codec-test` 校验帧编解码。

## mTLS 与 UDS

- mTLS：`--tls-ca` 让 B 要求客户端证书：C 侧校验证书 SAN 与 AUTH.devid 一致，
  A 侧校验证书 SAN 与 AUTH.user 一致（身份以证书为准，不接受自报）；
  C 侧用 `--tls-cert/--tls-key` 提供设备证书，`--tls-sni` 填 B 证书的主机名。
- UDS：`--socks5 unix:/path/to/socks.sock`，仅在使用 `-DENABLE_UDS=ON` 构建时可用
  （libhv 的 UDS 支持是编译期开关），默认仍是 `127.0.0.1:PORT`。

## 运维

- **权限即时生效**：账号/权限组/令牌的改动在管理台保存后立刻推给 relay，只影响新 OPEN，
  不动已建立会话；接入账号一律按权限组鉴权，未分组等于无权限。
- **优雅退出**：`SIGTERM`/`SIGINT` 停止 accept，等现有连接收尾（`--drain-timeout`，默认 10s）
  后退出；第二次信号立即强退。A/C 断线后按既有退避重连。
- **流空闲回收**：每 5s 扫描一次已建流，`--stream-idle` 秒内没有任何帧就向两侧发
  `RESET(stream idle timeout)` 并释放会话配额；端口映射这类正常空闲业务请调大或设 0 关闭。
- **统计**：每 60s 一条 `[relay] stats:` 日志（agents/clients/streams/bytes_in/bytes_out/frames/auth 失败）。
- **用量**：流量（token）、会话次数/时长（token）、设备在线时长（device）按 5s 增量写入
  `usage_events` 并累加 `usage_daily`；`GET /api/v1/usage` 按日或总计查询，需要 `usage.read`。
  「监控 → 用量」有报表页。原始流水保留 180 天（`usage.raw_retention_days`），日聚合长期保留。
  流量按载荷字节双向统计；管理台自身的终端/代理不计入客户用量。
- **带宽整形**：令牌可设 `rate_kbps`（kilobit/s，0 不限），relay 按令牌做令牌桶，
  超额时暂停数据来源侧的读（上行暂停客户端连接、下行暂停设备连接），靠 TCP 窗口回压，
  不丢帧；容量为 200ms 的量（最小 8 KiB），单次暂停不超过 500ms。适合按流量计费时
  防止单用户打满出口；管理台在「接入账号 → Token」里按 Mbps 填。
- **受控执行**：agent 用 `--allow-exec` 配白名单后，`POST /api/v1/agents/:devid/exec` 可执行
  argv（不经过 shell），stdout/stderr 分流、超时强杀、超字节截断；写审计 `device.exec`。
- **操作作业**：写动作走 `operations` 的 plan→apply→verify，`plan_hash` 防 TOCTOU、
  `Idempotency-Key` 防重放。risk ≥ 2 默认进入待审批，**必须由另一个账号批准**（自己批自己 403）；
  另有每分钟/每设备配额与 `HH:MM-HH:MM` 执行窗口。管理台「运维 → 作业」可批准/拒绝/执行/看详情。
- **事件流**：`GET /api/v1/events`（SSE）推设备上下线、会话建立/结束、审计、告警、任务状态，
  带 `id`（seq）支持 `Last-Event-ID` 续传；受限账号只收自己设备的事件。「概览」有实时事件面板。
- **分钟级指标**：relay 每分钟给在线设备要一份增量 sysinfo，落 `metrics_minute`（cpu/内存/负载/网卡），
  `GET /api/v1/agents/:devid/metrics/history?from=&to=&step=` 查趋势，默认留 7 天。
  「设备 → 采样曲线」里有「近 6 小时」曲线。
- **AI 助手**：**首页仪表盘直接内嵌**（同一组件全屏在「运维 → AI 助手」），问什么都能答：
  平台总览与告警、设备与会话、升级任务、租户与费用、客户账号、权限组、证书、中继、文件仓库；
  也能问“谁改过权限”“哪些证书快过期”“这个月多少钱”。
  也可以说“重启一下 nginx”“清理 /tmp 旧包”，它会生成一张**作业确认卡**（完整命令 + 风险级别），
  **不会自己执行** —— 只读命令点“执行”即可，改状态/破坏性命令要先“批准”，照样走配额与变更窗口。
  还能把排障结论写成**自愈规则草稿**，创建出来默认停用。
  共 19 个只读工具 + 2 个提议工具，工具**按账号权限裁剪**（没有证书权限就看不到证书工具），
  工具内部再按调用者的资源权限与设备范围收窄。
  **模型配置按账号隔离**：在「AI 助手 → 模型设置」选服务商（DeepSeek / Kimi / OpenAI / 通义 / GLM /
  本地 Ollama / 自定义网关）、确认模型名、贴 API Key，点「测试连接」即可（回显耗时、实际模型名、
  上游错误原文）。采样参数固定在服务端，不作为配置项暴露。
  **API 密钥只写到 relay 本机 `<admin.db 同目录>/ai/<账号>.key`（0600）**，不进数据库、不回显，
  别人登录看不到也用不到。支持任意 OpenAI 兼容接口。
  「概览 → AI 诊断」一键巡检：离线设备、版本落后、失败作业、告警汇总成排查建议。
- **使用指南**：「开放接口 → 使用指南」按「部署 → 接设备 → 权限 → 日常运维 → 接 AI」走一遍，
  命令可复制，纯前端页面（API 挂了也能看）。
- **自愈规则（playbook）**：把「告警 → 命令」固化成规则。规则只生成 `operations` 作业，
  自动执行需要三把锁同时成立：规则开了 `auto_apply`、命令风险 ≤ 系统设置
  `playbook.auto_apply_max_risk`（默认 1，只读）、全局 `playbook.enabled` 打开。
  另有规则冷却、每规则/全局每小时上限，执行单线程串行。管理台「运维 → 自愈」可配规则、
  看触发流水、立即执行。命令风险由服务端算，客户端改不了。
- **AI 接入（OpenAPI + MCP）**：`GET /api/v1/openapi.json` 在构建前端时从接口文档生成
  （文档由 `check:docs` 强制与路由表对表），接口变了它跟着变。`rtunnel/deploy/rt-mcp.py`
  把这份 schema 变成 MCP 工具，**默认只暴露只读接口**，写操作要 `--allow-write`，
  调用带 `X-Actor: ai:…` 进审计。用法见下节。
- **费用试算**：`price_rules` 存每个指标的计价单位与单价（金额 1e-6 元），
  `GET /api/v1/billing/summary` 把日聚合用量乘单价、按租户汇总；「监控 → 费用」可改价目
  （`billing.write`，默认只有超管）。
- **结算单**：按账期把金额与明细冻结成 `billing_statements`（draft → issued → paid，可作废），
  出账后不随用量变化；可导出 CSV 交给财务。仍未接支付与发票。
- **租户**：设备与接入账号各归属一个租户（默认租户 id=1），数据面按设备租户强制隔离，
  接入令牌只看得到、连得到本租户设备；停用租户等于停用其下全部接入账号并断开在线连接。
  租户可设套餐：设备上限（分配时校验）、并发上限、月流量上限（relay 数据面强制，超流量
  断开该租户全部令牌并拒绝新会话）；0 表示不限。
  详见 `../docs/design/rtunnel.md` §13.11。
- **自身指标**：`GET /metrics` 输出 Prometheus 文本，包含在线设备/会话/流、累计连接与认证失败、
  收发帧与转发字节、打开/失败/空闲回收的流、管理面 HTTP 派发耗时、SQLite 语句耗时与慢查询、
  告警队列和分发任务数。需要 `device.view` 资源（用 `atk-` Token 最方便）：

  ```yaml
  scrape_configs:
    - job_name: rtunnel
      scheme: https          # 明文管理台改 http
      authorization:
        credentials: atk-xxxxxxxx   # 权限组需包含 device.view
      static_configs:
        - targets: ["relay.example.com:9443"]
  ```

  `/metrics` 是 relay 自己的指标；设备侧的 CPU/内存曲线在
  `GET /api/v1/agents/:devid/metrics/series`，两者不混用。

## 管理台（admin）

relay 启动加 `--admin 127.0.0.1:9443` 开启管理台（libhv HttpService，独立 worker 线程）：

- 能力：概览、设备/会话、踢会话/下线、受控命令执行与操作作业、权限（权限组 / 授权对象 / 资源与设备字典）、接入账号与 Token（时效 / 流量 / 频率 / 并发 / 带宽）、租户、用量与费用试算、设备文件与服务管理、证书签发/吊销、文件仓库、分发与 OTA、审计、系统设置
- 资源键与数据面能力一一对应：`device.view` 只读可见，`device.svc` 才能启停服务，
  串口分 `device.tty.read`（只读流）与 `device.tty.write`（写入/接管）。令牌吊销、账号停用、
  权限组变更会立即断开该主体的在线连接；管理台的终端/串口/文件/服务页面在权限变小、
  账号停用或令牌失效时也会被断开（扩容不打断），需要刷新页面重新握手。
  审计记录真实操作者（账号 + 令牌 + IP）。
- 资源分「设备级」与「全局」两类：仓库 `repo.*`、中继 `relay.*`、字典/权限组/账号/令牌/证书/
  审计/系统设置属于全局资源，需要权限组开启「全局权限」（勾了「全部设备」或 `super` 自动视为开启）。
  这样受限设备的委派管理员也能管理用户/权限组/仓库，但不能借全局键影响范围外的设备。
- 接入账号与令牌的写操作要求归属账号当前权限在自己范围内（令牌继承账号权限，轮换等于
  重新签发）；启停设备资源要求设备在范围内。
- 令牌第一次绑定权限组后即成「显式」：组被清空或所在组被删除等于无权限，不会回落到归属账号；
  给主体换组、改组成员、组页面保存与删除组时，目标当前权限也必须在自己之下。密码至少 8 位，
  用户名不允许控制字符，首尾不能是空格（中间允许）。
- 侧栏按用途分 5 组，组可折叠（默认只展开当前页所在组，空白组按角色隐藏）：
  监控（概览/设备/会话）、运维（文件/分发/任务/Agent 升级）、
  权限（权限/接入账号/客户端/账号）、开放接口（API Token/接口文档）、
  系统（证书/审计/设置）
- 接口文档：`#/docs` 按当前角色列出可调用接口，支持搜索与「只显示我可调用」，
  并给出 curl / WebSocket 示例；`cd relay/admin/web && npm run check:docs`
  会校验文档与 `admin_routes.cpp` 的路由是否对表
- API 列表：见 `../docs/design/rtunnel.md` §13.3；页面内 `#/docs` 会按角色列出可调用项
- 文件分发：文件页「分发」→ 选择设备/时间窗/重试 → 每设备一条任务；
  `POST /api/v1/distributions` 创建，`GET /api/v1/distributions` 查询，
  任务页支持状态过滤、重试、取消、超时延长
- 文件哈希：上传即计算 sha256；同设备同哈希重复分发标记「冲突」，
  可校验设备文件（相同则直接成功）或强制重发
- 前端：构建 `rtunnel/relay/admin/web` 后，用 `--admin-docroot DIR` 指向 `dist/`；
  未指定时管理台页面不可用（仅 API 正常）。不想带目录时用内嵌版：
  `cmake .. -DRTUNNEL_ADMIN_WEB=ON && cmake --build . --target rtunnel-relay-embedded`，
  换出的二进制自带前端（构建期把 `web/dist` 的预压缩体嵌入，体积约 +0.8 MB）；
  指定 `--admin-docroot` 时仍以磁盘为准，磁盘缺文件再回退内嵌
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
- 线程模型：管理面 HTTP 默认 4 个 worker（`--admin-threads N`，1..32），relay 状态访问经
  `hloop_post_event` 投递回 relay 线程执行（promise/future 同步），relay 数据无锁；
  `/device` 的 HTTP 代理每个请求一条独立线程（上限 `proxy.max_concurrent`），终端/文件/服务
  WebSocket 各有自己的读线程，管理面 worker 不会被长连接或慢设备拖住；分发/告警/清理等
  后台任务由 `AdminServer` 统一管理，`stop()` 时置标志、收掉代理线程并 join 全部 worker。

## 账号与权限（SQLite）

管理台账号存 SQLite（`--admin-db`，默认 `rtunnel-admin.db`），**需要 OpenSSL 构建**（PBKDF2 口令哈希）。

- **首次部署**：浏览器打开管理台 → 初始化页输入 `--admin-token` + 管理员用户名/密码 → 创建 admin；
  初始化前 token 只能访问 `/api/v1/setup*`
- **之后登录**：用户名 + 密码，服务端会话（HttpOnly Cookie，默认 12h，可在设置中调整），不再用 token
### 资源权限与权限组

管理台每个动作对应一个资源键（`perm_resources`，40 项），权限判断按资源、设备与端口求值：

- **功能资源**：`perm_resources`（代码登记）+ `perm_group_resources`（组勾选）
- **设备资源**：`res_devices`（agent 上线自动登记）+ `perm_group_devices`（组授权）
- 权限组 `perm_groups` 额外有 `all_devices`（全部设备，含以后新增）、`global_scope`
  （全局权限，见下）和 `ports`（组级 TCP 端口范围）
- 组成员 `perm_group_members.subject_type` 有四种：`user`（管理台账号）、`atk`（管理台 Token）、
  `client`（接入账号）、`rtk`（接入 Token）
- 账号 / 接入账号的有效权限 = 所属权限组的资源并集、设备并集
- **令牌只能收窄**：atk / rtk 的有效权限 = 归属账号（或接入账号）∩ 令牌所属组；
  令牌没有组时完全继承归属；令牌永远不能超过归属账号
- **账号权限只来自权限组**：`users.role` 只是内置组的别名，新建账号时自动加入该角色对应的
  内置组；之后组成员变化会把 `users.role` 同步成最高的内置组（没有内置组时归 viewer）。
  不存在“未入组按角色回落”的路径，移出全部组就是零权限
- 设备不接受直接绑定到账号：账号、atk、接入账号、rtk 只能通过权限组的设备授权获得设备范围
- 超级权限来自「超级管理员」组（`super=1`），没有 `role=admin` 的硬编码；超级组至少保留
  一个账号成员，内置角色组不能删除
- 预置 4 组：超级管理员（`super=1`）、管理员、操作员、观察者；首次启动写入一次，
  后续勾选结果不会被重启覆盖
- 资源分类：设备（查看/终端/端口转发/能力/OTA/串口/采样/断开/进程查看/进程控制）、文件（读/写/删除）、
  分发（读/写）、权限（读/写）、账号（读/写/删除）、令牌（读/写）、证书（读/写）、
  审计、系统（读/写/关机）
- API：
  - `GET /api/v1/permission/resources`、`PUT /api/v1/permission/resources/{key}`
  - `GET /api/v1/permission/groups`、`GET /api/v1/permission/groups/{id}`
  - `POST /api/v1/permission/groups`、`PUT /api/v1/permission/groups/{id}`（资源/设备/成员一次保存）
  - `GET/PUT /api/v1/permission/groups/{id}/members`
  - `GET /api/v1/permission/subjects`、`GET/PUT /api/v1/permission/subjects/{type}/{id}/groups`
  - `GET /api/v1/permission/dicts?kind=resources|devices|ports|ttys`（分页 5/10/20/50，默认 10）
  - `PUT /api/v1/permission/devices/{devid}`（改名 / 临时停用）
- 页面：「权限组」用成员/功能资源/设备资源三个 tab；功能资源是分组矩阵，成员与设备用穿梭框；
  「授权对象」按账号/atk/接入账号/rtk 查看并分配权限组；「字典库」管理四类字典
- 菜单与按钮按 `/api/v1/me` 返回的 `perms`/`super` 显示，服务端始终二次校验

**atk 与 rtk 的区别**（两者都是令牌，但不是同一种身份）：

| | atk（管理台 API Token） | rtk（接入 Token） |
|---|---|---|
| 归属 | `users` 管理台账号 | `client_accounts` 接入账号 |
| 使用场景 | 管理台 HTTP `Authorization: Bearer atk-…` | relay 客户端；也可调管理台 HTTP |
| 权限公式 | 归属账号 ∩ 令牌组 | 接入账号 ∩ 令牌组 |
| 数据面 | 不参与 | 账号级规则 + 令牌级规则；令牌配了组就只认令牌规则（空组 = 拒绝） |
| 创建/管理 | 「API Token」页 | 「接入账号」详情页 |
| 授权 | 「授权对象 → atk 令牌」或权限组成员 | 「授权对象 → rtk 令牌」或权限组成员 |

一句话：**atk 是“账号的管理台钥匙”，rtk 是“接入账号的设备钥匙”**；两者加入权限组都只能收窄，
不能超过归属主体。

`users.role` 只决定新建账号进入哪个内置组，以及账号管理页的管理层级（不能操作比自己高的
角色）；权限本身仍由组求值（admin 唯一；manager/operator/viewer 可多个）：

| 角色 | 初始内置组 | 预置资源 |
|---|---|---|
| admin | 超级管理员（super） | 全部资源 + 全部设备 |
| manager | 管理员 | 除系统设置、关机、证书写、删账号外的全部资源（全局权限默认开启） |
| operator | 操作员 | 设备查看/终端/端口转发/服务/串口读写、文件读写、分发读、仓库读写、权限读、账号读、设置读（全局权限默认开启） |
| viewer | 观察者 | 设备查看、串口读、设置读（全局权限默认开启） |
- **账号 API**：`GET/POST /api/v1/users`、`PUT /api/v1/users/{id}/password|status`、
  `DELETE /api/v1/users/{id}`；`POST /api/v1/login|logout`、`GET /api/v1/me`
- 口令策略：默认最短 8 位、连续 5 次失败锁定 5 分钟（均可在「设置」调整）；改密/禁用会吊销全部会话

### API Token（atk-）

管理台账号可创建 API Token，用于脚本/CI 调用管理台 HTTP 接口，不必保存账号密码或会话 Cookie。

- 格式 `atk-<64 位十六进制>`（32 字节随机数的 SHA-256）；库存 SHA-256 与前缀，
  明文只在创建/轮换时返回一次，之后只能轮换
- 请求头 `Authorization: Bearer atk-…`，与浏览器 Cookie 会话互不影响；Token 继承所属账号的权限组
- 权限组：在「授权对象」页把 Token 加入权限组。有效权限 = 归属账号 ∩ Token 组，
  令牌只能收窄不能放大；不加入任何组时完全继承归属账号
- 管理台里 manager 只能管理自己的 Token，admin 可管理全部；只有 admin 能修改分组绑定
- Token 不能管理 Token（避免凭据自我复制）；停用 Token、停用/删除归属账号或到期后立即失效；
  轮换会替换哈希并让旧 Token 失效
- API：`GET/POST /api/v1/admin-tokens`、`PUT /api/v1/admin-tokens/{id}`、
  `POST /api/v1/admin-tokens/{id}/rotate`、`DELETE /api/v1/admin-tokens/{id}`；
  权限组用 `GET/PUT /api/v1/permission/subjects/atk/{id}/groups`
- 页面：「API Token」导航页（需要 `token.read`，创建/轮换需要 `token.write`）

```bash
# 创建后只显示一次的明文
TOKEN=atk-...
curl -H "Authorization: Bearer $TOKEN" https://relay:9443/api/v1/agents

# 也可以直接上传文件到文件仓库（需要 operator 及以上角色）
curl -H "Authorization: Bearer $TOKEN" --data-binary @app.tar.gz \
     "https://relay:9443/api/v1/files/app.tar.gz"
```

## 设备系统信息与能力

设备详情页（operator 及以上，viewer 可看自己名下设备）：

- 单层 tab：概览 / 存储 / 文件 / 服务 / 进程 / 网络 / 监控 / 串口 / 能力；
  非活动 tab 不挂载、不请求，首次进入才加载，之后保留状态
- 一级 tab 同步到 URL 的 `?tab=`，刷新后保持当前 tab；浏览器前进/后退可切换 tab
- 「存储」tab 每个分区可折叠：展开后只取分区根的一级子项，点开某个目录才按
  路径请求它的直接子项，右键「全展开」才按需拉整棵子树。目录按缩进 + 条形
  画成瀑布（条长按分区容量归一），子项过多合并为「其他」；目录与大文件都
  单独成行，小于 64 KB 的文件按目录聚合为「小文件 N 项」，因此每一层子项
  之和等于父项；工具栏眼睛按钮切换是否显示以 `.` 开头的项；已取回的节点
  缓存 5 分钟，收起再展开不重复扫描。agent 把整份响应压成 gzip 后上传，
  relay 对支持 gzip 的浏览器原样透传压缩流（由浏览器解压），
  不支持时由 relay 解压后回明文。这份树的管理台与设备本地页共用
  `shared/web/storage-ui.js`，只有数据来源不同
- 概览：CPU/内存/负载/网络累计流量四个 KPI、CPU/内存趋势、存储用量、网卡、
  会话、系统信息与 mTLS 证书状态
- 顶部摘要条展示设备名、agent 版本、连接信息、流量与采集时间，和一级 tab 一起吸顶
- 「能力」tab 展示 TCP 端口、Shell、SYSINFO、mTLS、OTA、可重启/可关机、文件/服务能力与串口白名单；
  管理台有 `device.caps.write` 的账号可在线编辑，设备本地页只读
- 表格默认 10 行/页，支持 5/10/20/50；进程、服务、网络（TCP/UDP、Unix、IPC）和
  分发/OTA 任务都在服务端排序后再分页，只回传当前页；网卡/串口和文件列表在浏览器端排序。
  管理台和设备本地页共用 `components/DeviceView.vue`，都用 Element 的
  `sortable` / `sortable="custom"` 与 `@sort-change`；数值列默认降序、文本列默认升序
  （`shared/tableSort.js` 提供 `NUM_ORDERS` / `TEXT_ORDERS`）。共享渲染组件
  （`fs-ui` / `svc-ui`）内部的表格仍走 `shared/web/table-ui.js`
- 文件/服务 tab 首次打开时建立会话；进程 tab 支持 1/2/3/5/10/20/30 秒自动刷新
- 监控 tab 内切换「曲线 / 采样配置」；串口配置独立为一级 tab
- 监控「指标」下拉里可「添加进程…」：选中进程的 CPU/内存后才开始采样，每个进程两条曲线
  （`proc:<pid>:cpu` / `proc:<pid>:mem`，默认 1 s 一个点、保留 30 分钟）。CPU 折成整机占比，
  和系统 cpu 同一标尺；内存为占总内存百分比。进程曲线单独成图，避免被系统内存压成贴底直线；
  取消进程指标 chip 会下发 `enabled=false`，立即停止该进程采样；agent 同时清掉 ring、
  采样时刻和 CPU 增量状态，内存归还。系统指标（cpu/mem/load/net/temp）取消 chip 只隐藏曲线，
  仍按配置采样。停用的进程指标保留在「采样配置」里，打开开关即可恢复。
  指标 chip 的勾选按设备记在浏览器本地，刷新或换设备后进程曲线仍然显示；本地页的采样配置只读，
  修改入口只在管理台

采集与配置：

- `SYSINFO_REQ(0x19)` / `SYSINFO(0x1A)`：按需采集，relay 仅内存缓存，不落库
- `CONFIG_SET(0x1B)` / `CONFIG_ACK(0x1C)`：在线修改端口、Shell、OTA、文件/服务能力与串口白名单，
  agent 原子生效并写入配置文件，重启后保持
- 能力配置加载顺序：`--config-file` 指定文件（默认 `/opt/data/etc/rtunnel-agent.json`）→
  文件不存在时使用命令行参数；文件中未出现的字段沿用命令行值，文件中的值优先。
  首次在线修改能力或采样配置时创建该文件；采样配置也随能力配置一起落盘
- relay 通过 `SYSINFO` 的 `caps` 获取 agent 的**运行时**能力，设备详情页「能力」tab
  直接展示该结果（含配置文件路径、是否从文件加载、端口/OTA/文件/服务/串口白名单）
- API：`GET /api/v1/agents/{devid}/sysinfo?mode=full|inc`、`PUT /api/v1/agents/{devid}/caps`
  （设备详情页「存储」的百分比按 已用 / 总量 计算，与同屏数值一致；`avail_kb` 是
  非特权用户可用空间；同一文件系统的 bind mount 只上报最短路径；设备离线或回帧超时时返回带 `cached:true`
  的缓存快照，等待回帧在后台线程进行，不阻塞管理台其它请求）
- 网络与 IPC 快照：`GET /api/v1/agents/{devid}/net?scope=tcp|udp|unix|ipc|all&proc=1&page=1&page_size=10`
  （agent 读 `/proc/net/{tcp,tcp6,udp,udp6,unix}` 与 `lsipc -m|-q|-s -J`，
  缺失 lsipc 时回退 `/proc/sysvipc`；IPC 额外扫 `/dev/shm` 上报 POSIX 共享内存
  （`ipc_kind=posix`），否则只跑 SysV 的机器会看起来完全没有 IPC 对象。
  `proc=1` 扫 `/proc/<pid>/fd` 反查占用进程。
  过滤、分页在 agent 完成，只回传当前页；relay 缓存 3s，`refresh=1` 绕过）
  网络 tab 把网卡与 TCP/UDP/UNIX/IPC 放在子 tab；网卡数据来自
  sysinfo（名称/IP/掩码/MAC/MTU/速率/收发字节/错误），在页面本地分页与搜索。
- 进程：`GET /api/v1/agents/{devid}/procs?sort=cpu|mem|pid|start&q=&page=1&page_size=10`、
  `GET .../procs/{pid}`、`POST .../procs/signal {pid,signal}`
  （agent 扫 `/proc/<pid>/{stat,status,cmdline}`，CPU 首次用 300ms 双采样，之后用两次
  快照的累计 CPU 时间差计算，窗口就是刷新间隔；
  PID 1 与 agent 自身拒绝发信号；分页与排序在 agent 完成，只回传当前页。查看需要
  `device.proc.read`，信号需要 `device.proc.signal`，两者都受账号设备范围限制。
  打开设备详情页时 relay 下发 `WARM_REQ`，agent 在页面打开期间每 30s 预采样进程与
  服务列表；随后的翻页/搜索直接命中缓存（`cached:true`），不再等 300ms 双采样）
- 电源操作：`POST /api/v1/agents/{devid}/power
  {action:"reboot"|"poweroff",delay_ms,password}`，必须带设备 root 密码；
  agent 用 `/etc/shadow` 的哈希校验，连续 5 次失败冷却 5 分钟。
  agent 侧分 `"reboot"` / `"poweroff"` 两个开关（能力页可分别开启），
  ACK 先返回，延迟到点再执行，权限资源 `device.power`。默认关闭
- 目录占用：`GET /api/v1/agents/{devid}/storage?mount=/&path=/usr&depth=1`
  只回传 `path` 下 `depth` 层内的节点，展开某个目录时再按它的路径请求，
  不会一次上传整棵树；`path` 为空表示分区根。
  node = `{p,b,n,d,m,t,c}`：路径/字节/文件数/子目录数/修改时间/类型/是否还有
  子项；`t` 为 `d` 目录、`f` 文件、`o` 小文件聚合。目录按层级回传，文件只回传
  >= 64 KB 的，其余按目录聚合成一条，保证子项之和等于父项；
  agent 端走共享阻塞 worker，与服务管理串行，不占用进程/网络查询队列；
  同一 `mount/path/depth` 的结果 60s 内复用（`force=1` 强制重扫）；
  单次上限 20 万目录项 / 20s，
  跳过无权限目录与其它分区，只读元数据，权限复用 `device.view`
- 诊断包：`GET /api/v1/agents/{devid}/diag` 返回 `application/gzip` 附件
  （agent 汇总 sysinfo、dmesg、journal 500 行、`ip addr/route`、`ss -s`、服务列表、
  前 30 进程、网络快照与本机配置，打成 tar.gz；生成走查询 worker，上限 8MB，
  权限资源 `device.diag`）
- 文件分发：文件页「分发」选择设备与时间窗，创建 `POST /api/v1/distributions`，
  `GET /api/v1/distributions` 查询进度；经 RTNP 推到设备 `--file-dir/dir`
  （默认 `/opt/data/files`），32KiB 分片、256KiB 窗口、sha256 校验、断点续传、
  冲突校验；`.part` 超过 24h 自动清理
  （端口与 Shell 开关都看 `device.caps.write`；设备离线直接报错）

## 部署包与自举（relay / agent / client）

三个组件同一套包格式与命令，目标机只要有一个 `rtunnel-client` 就能升级后续所有组件
（含它自己），不再依赖 curl/scp。设计见 `../docs/design/deploy-packages.md`。

```bash
# 打包（解压出一层 rtunnel-<component>-<ver>-<arch>/，含 bin/ etc/ service/ install.sh）
./deploy/build_packages.sh <ver> --arch x86_64
./deploy/package_agent.sh <ver>                 # agent 的 lite/web 变体

# 发布到文件仓库（幂等：同 sha256 跳过）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./deploy/publish.sh --component all --version <ver> --arch x86_64

# 目标机自举：取 latest → 下载 → 校验 → 解包 → install.sh
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rt-bootstrap.sh --component relay --version latest --arch x86_64 --start

# 日常
/opt/rtunnel/relay/install.sh --status
/opt/rtunnel/relay/install.sh --rollback --restart

# 无 systemd（容器 / /tmp 部署）：用包内 restart.sh，读 $PREFIX/etc/relay.env
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
# 开机自启 + 每 5 分钟存活看护（crontab，重复执行只替换自己的行）
/tmp/rtunnel/relay/install-boot.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
```

仓库布局 `packages/<component>/<ver>/<arch>/<pkg>.tar.gz`，指针
`packages/<component>/latest/<arch>.json`（agent 为 `<arch>-<variant>.json`）。
`install.sh` 只做文件操作：先校验 `manifest.json`、`--version` 与 `SHA256SUMS`，再原子切换软链；
`--start/--restart` 才动 systemd，重启失败自动回滚一次；无 systemd 时改调 `restart.sh`，
不会再“假装启动成功”。

## AI 接入（OpenAPI / MCP）

```bash
# 1) OpenAPI：不需要令牌，和首页同级
curl -k -sS https://relay:9443/api/v1/openapi.json | jq '.info, (.paths|length)'

# 2) 工具清单（默认只读）
python3 rtunnel/deploy/rt-mcp.py --rt https://relay:9443 \
  --atk-file /etc/rtunnel/atk --insecure --list-tools

# 3) 作为 MCP server 挂在客户端下（Claude Desktop / 自研 agent 都是这个形状）
python3 rtunnel/deploy/rt-mcp.py --rt https://relay:9443 \
  --atk-file /etc/rtunnel/atk --insecure
```

客户端配置：

```json
{"mcpServers": {"rtunnel": {"command": "python3", "args": [
  "/opt/rtunnel/relay/deploy/rt-mcp.py",
  "--rt", "https://relay:9443",
  "--atk-file", "/etc/rtunnel/atk", "--insecure"]}}}
```

约束与约定：

- **schema 单源**：`api-docs.js` →（构建期）`dist/openapi.json` →（运行期）MCP 工具。
  不手写第二份，接口变了工具自动跟上。
- **默认只读**：只有 GET 变成工具；`--allow-write` 才暴露写接口，`--allow-prefix` 可再收窄。
- **不绕过授权**：工具只是 API 的包装，能不能调仍由令牌的资源键与设备范围决定；
  写操作建议走 `operations` 的 plan→apply→verify（可配审批），不要把 `exec` 当万能口子。
- **审计可区分**：MCP 发出的请求自动带 `X-Actor: ai:rt-mcp`（可用 `RT_ACTOR` 覆盖），
  审计里显示成 `ai:rt-mcp|账号#id/…`，事后能分清人和模型。
- **令牌只从文件/环境读**：`--atk-file`（0600）或 `ATK`，不接受命令行明文。

## Agent OTA 升级

OTA 不另建传输通道：部署包走「文件上传 + 文件分发」落到设备 `--file-dir`，
分发完成后 relay 下发安装指令，统一记录在 `distribution_tasks`：

1. `deploy/package_agent.sh` 生成两个部署包：
   `rtunnel-agent-lite-<ver>-<arch>.tar.gz` / `rtunnel-agent-web-<ver>-<arch>.tar.gz`。
   解压目录是 `rtunnel-agent-<ver>-<arch>/`，包内固定 `bin/rtunnel-agent`、`bin/agent-boot-check.sh`、
   `bin/rtunnel-client`、`etc/rtunnel-agent.json`、`service/rtunnel-agent.service`、
   `manifest.json` 与 `SHA256SUMS`；variant 只体现在文件名与 manifest；
   版本取二进制 `--version`，架构取 `uname -m`（交叉编译用 `RTUNNEL_ARCH` 指定）；
2. `deploy/publish.sh --component agent --variant web` 用 `rtunnel-client repo` 上传到
   文件仓库 `packages/agent/<版本>/<架构>/`（需要 atk 令牌），
   `/ota` 页按文件名递归搜索该目录，解析 variant、版本和架构，并按架构过滤设备；
3. 传输复用 `FILE_BEGIN/DATA/END`（0x1D-0x1F），落盘设备侧 `packages/agent/<版本>/<架构>/xxx.tar.gz`；
4. relay 发 `OTA_APPLY`（0x30，JSON：path/version/size/sha256）：
   agent 校验大小与 sha256 → 解压 → 解包到 `.ota/`（拒绝 `..`、绝对路径、符号链接）→
   manifest（版本/架构）与 `--version` 自检（`verifying`）→ ACK `verified`（`installing`）；
5. agent 执行包内 `install.sh`：装版本化二进制和 boot-check、更新 unit、
   配置不存在时安装默认配置、写 `state/previous` 与 `state/pending`，
   最后原子切换 `bin/rtunnel-agent -> rtunnel-agent-<variant>-<ver>`；
6. ACK `installed`，等 ACK 写出后排队 `systemctl --no-block restart rtunnel-agent.service`
   并退出；unit 的 `Restart=always` 兜底（`confirming`）；
7. 新 agent 启动时 `ExecStartPre=agent-boot-check.sh` 计数；AUTH_OK 后清 pending、
   上报 SYSINFO（含版本和 `caps.arch`），relay 在 `confirming` 阶段比对目标版本 → `success`。

任务状态：`scheduled→queued→running→verifying→installing→confirming→success`；
失败分支为 `failed / timeout / conflict / unsupported / canceled`，重试沿用分发任务的
退避与截止时间。OTA 与普通分发任务都在「任务」页查看，可按 `kind=ota` 过滤。

失败与回滚：

- 包校验/解包/`--version` 自检失败：不动现场，旧版继续跑；
- `install.sh` 失败：软链还没切换（切换是最后一步），旧版继续跑；
- 新版本连续 3 次没起来，或每次启动 45s 内没连上 relay：`agent-boot-check.sh` 把软链切回
  `state/previous`，任务因版本不匹配显示 `version not confirmed`；
- 手动回滚：`/opt/rtunnel/agent/install.sh --rollback`，然后重启服务；
- 设备只保留当前和上一个二进制（回滚只需这两个），更早的版本在安装时清理；
- 查看当前/上一版本：`/opt/rtunnel/agent/install.sh --status`。

设备侧前置条件与限制：

- 配置里 `"ota": true`（部署包自带 `/opt/data/etc/rtunnel-agent.json` 默认开启）；
- 只对常驻 agent 生效，按需 agent（`a:<user>`）拒绝；
- `/opt/rtunnel/agent/bin` 和 `/opt/data/etc` 必须可写；
- 升级会重启 agent，隧道/shell 会断开；
- 版本确认超时 `ota.apply_timeout_s`（默认 180s）。

OTA 只接受部署包（`.tar.gz`）。项目还在开发阶段，不提供旧二进制/旧目录布局的自动迁移：
设备统一按下面的布局安装，旧开发机直接重装即可。

```bash
# 首次安装：先写身份与 relay 地址
mkdir -p /opt/data/etc
cat > /opt/data/etc/rtunnel-agent.args <<'EOF'
AGENT_ARGS=--devid board01 --server 10.0.0.10:18443 --allow 22
EOF

# 解包并安装（--start 首次安装会 enable --now）
tar -xzf rtunnel-agent-lite-<ver>-<arch>.tar.gz
sh rtunnel-agent-<ver>-<arch>/install.sh \
  --pkg-dir rtunnel-agent-<ver>-<arch> \
  --variant lite --start
```

设备布局固定为 `/opt/rtunnel/agent`，unit 不解析额外参数：
`/opt/data/etc/rtunnel-agent.args` 只提供 `AGENT_ARGS`，能力开关在
`/opt/data/etc/rtunnel-agent.json`。

```bash
# 生成 lite + web 两个包（版本默认取部署二进制的 --version，架构默认 uname -m）
./deploy/package_agent.sh 1.4.9

# 交叉编译（ARM）：用 qemu 跑目标二进制做 --version 自检，并显式指定目标架构
RTUNNEL_DEPLOY_DIR=build/deploy_mp135 RTUNNEL_ARCH=armv7l \
RTUNNEL_PKG_RUNNER="qemu-arm-static -L /opt/st/.../sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi" \
./deploy/package_agent.sh 1.4.9

# 上传（架构要与打包时一致）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk RTUNNEL_ARCH=armv7l \
  ./deploy/publish.sh --component agent --variant web --version 1.4.9 --arch armv7l
```

容器或临时目录里试装时，可加 `RTUNNEL_INSTALL_NO_UNITS=1`（或 `--no-units`）
让 `install.sh` 完全不碰 `/etc/systemd/system` 和 `systemctl`。

API：`POST /api/v1/distributions` 增加 `kind: "ota"` 与 `version`、`force` 字段；
查询复用 `GET /api/v1/distributions?kind=ota` 与 `.../summary`，重试/取消复用
`POST /api/v1/distributions/:id/retry|cancel`。

## Web 终端（免密）

「设备 C」「客户端 A」列表的 Shell 入口：admin 作为 RTNP client 直连本机 relay，
向目标 agent 发 `OPEN_SHELL`（flags=0x0001），agent `forkpty` 起交互 shell，
经 WebSocket 接到浏览器 xterm.js。不 fork ssh、不经手密码。
前端支持多 tab；切换页面只摘 DOM，不关闭 WebSocket 会话。

- 身份固定为管理台内部代理（relay 启动时生成的进程内密钥，不落盘、不配置），
  **不接受浏览器指定用户**；操作者权限与设备范围在管理台侧校验；
- 设备列表：目标 devid = 设备 ID；客户端列表：目标 devid = `a:<user>`（按需拉起，见下节）；
- 需要目标 agent 开 `--allow-shell`；
- 终端尺寸经 `RESIZE`(0x17) 帧同步。
A 端也可在本机执行 `rtunnel-client --server B --token rtk-... shell <devid>` 直连设备免密 shell。

## A 侧按需 Shell（client 列表的 Shell 按钮）

client 加 `--allow-shell`（默认关）后：

1. 管理台「客户端 A」点 Shell，admin 对 `a:<user>` 发 `OPEN_SHELL`；
2. relay 发现 `a:<user>` 未注册且该 user 的 client 在线，向其下发 `SPAWN_AGENT`，并挂起 OPEN（10s 超时）；
3. client 在本机 `fork/exec` 同目录的 `rtunnel-agent`（可用 `--agent-path` 指定），
   以 `a:<user>` 注册；relay 把挂起的 OPEN 继续转发给这个 agent；
4. 无会话空闲 `--agent-idle` 秒（默认 15）后 agent 自行退出，client 退出时也会回收。

client 侧只做「拉起/回收」，shell 逻辑完全复用 agent；relay 侧只多一个挂起状态和新控制帧，
不需要在 A-B 连接上叠双角色。

```bash
build/bin/rtunnel-client --server B --port 8443 --token rtk-... \
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
| `--allow-all` | 跳过权限组鉴权（仅本地测试） |
| `--tls-cert/--tls-key [--tls-ca]` | 服务端 TLS；给 `--tls-ca` 即要求客户端证书（mTLS） |
| `--admin ADDR --admin-token T` | 管理台监听；token 仅用于首次初始化 |
| `--admin-threads N` | 管理面 HTTP worker 数，默认 4，范围 1..32 |
| `--admin-db PATH` | SQLite 账号库，默认 `rtunnel-admin.db`（需 OpenSSL 构建） |
| `--admin-ca-cert/--admin-ca-key/--admin-cert-dir` | 证书签发 CA 与台账目录 |
| `--admin-tls-cert/--admin-tls-key` | 管理台 HTTPS：使用已有证书 |
| `--admin-tls-selfsigned [--admin-tls-dir D] [--admin-tls-days N]` | 管理台 HTTPS：自签并自动续期 |
| `--admin-file-dir` / `--admin-docroot` | 文件管理目录 / 前端磁盘目录（内嵌版可省略） |
| `--drain-timeout` | 优雅退出等待时长，默认 10000ms |
| `--stream-idle SEC` | 流空闲回收：超过 SEC 秒没有任何帧就 RESET 并释放会话配额，默认 1800，0 关闭 |

relay 每 5s 巡检一次 agent 控制连接：连续 60s 没有收到任何帧就断开并标记离线
（agent 每 15s 发一次 PING）。设备断电/断网但 TCP 还挂着的「假在线」会在一分钟内收敛。

**client（A）**

| 参数 | 说明 |
|---|---|
| `--server/--port --token` | relay 地址与 `rtk-` 令牌（身份由令牌决定）；`--server` 可重复或逗号分隔（最多 8 个，故障自动切换） |
| `--socks5 HOST:PORT` / `unix:PATH` | 本地 SOCKS5 入口；**可省略**，省略时只做 `--forward` / `shell`，不会再默认占用 1080 |
| `--forward 127.0.0.1:16022=devid:22` | 本地端口映射，可多次 |
| `--allow-shell` | 允许 B 通知本机按需拉起 agent（默认关） |
| `--agent-path` / `--agent-idle` | agent 路径 / 空闲自退秒数（默认 15） |
| `--tls-ca/--tls-cert/--tls-key/--tls-sni` | TLS / mTLS |
| `shell <devid>` | 本机终端直连设备免密 shell |
| `repo <mkdir\|put\|get\|stat\|ls\|rm>` | 直接读写管理台文件仓库（HTTPS + atk-，不走隧道）；`put/get` 支持断点续传，`get` 落盘后校验 sha256 |
| `--version` | 输出版本号（打包、自举与安装自检都用它） |

**agent（C）**

| 参数 | 说明 |
|---|---|
| `--server/--port --devid` | relay 地址与设备 ID；`--server` 可重复或逗号分隔（最多 8 个，故障自动切换） |
| `--allow 22,80` / `--allow-all` | 目标端口白名单 |
| `--allow-shell` / `--max-idle` | 允许 `OPEN_SHELL` / 空闲自退秒数 |
| `--allow-exec "cmd a *;cmd b"` | 受控命令执行白名单（分号分隔的 argv 模板，逐段通配）；不配则 `EXEC` 不可用 |
| `--exec-timeout` / `--exec-max-bytes` | 单次执行超时（默认 30000ms）与 stdout+stderr 合计上限（默认 256 KiB） |
| `--allow-ota` | 允许 relay 通知安装 file_dir 中的新版本（命令行默认关；部署包自带配置为开） |
| `--local-web [ADDR:]PORT` | 本地设备详情页（内置单文件 Web），默认 `127.0.0.1:8080` |
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
  `:8080` / `0.0.0.0:8080` 监听所有网卡。页面包含文件、服务等**可写**操作，
  同源检查只防浏览器跨站，不能代替鉴权：对外监听时必须同时配置
  `--file-token` / `--svc-token`，否则同网段客户端可以直接读写文件和控制系统服务
- 页面与管理台设备详情是同一份 Vue 源码（`relay/admin/web/src/components/DeviceView.vue`），
  本地入口在 `src/local`，通过 API 适配直连 agent；`npm run build:local` 构建成
  单个 HTML 后写入 `agent/web/index.html`，由 CMake 内嵌，不加载外部页面资源
- 单层 tab（概览 / 存储 / 文件 / 服务 / 进程 / 网络 / 监控 / 串口 / 能力），
  概览含 CPU/内存/负载/网络 KPI、趋势、存储、
  网卡、系统信息（温度、eMMC/SD 寿命、mTLS 证书）；本地页没有 relay 会话，
  概览不显示「会话」卡，能力/采样/串口配置为只读；存储页分区可折叠，
  展开后按 `GET /api/v1/storage/tree` 拉目录树并画成瀑布；非活动 tab 不请求数据，
  首次进入才加载
- 本地页与 relay 详情共用同一套预热：agent 启动和页面打开时刷新进程/服务缓存，
  页面打开期间由 2s 的 sysinfo 轮询续期（关闭后 90s 停止），
  服务列表不用再等 `systemctl` 首次全量扫描（单核设备约 4s）
- HTTP 接口：`GET /`、`GET /api/v1/sysinfo`、`GET /api/v1/procs`、
  `GET /api/v1/net`、`GET /api/v1/metrics`、`GET /api/v1/storage/tree`
  （`mount` / `path` / `depth`，按目录按需返回，`force=1` 跳过 60s 缓存）、`GET /api/v1/health`；
  文件与服务管理走 `GET /ws/fs`、`GET /ws/svc`（WebSocket，见下节）
- `procs` / `net` / `metrics` 与 relay 的 `PROC_REQ`、`NET_REQ`、`METRICS_QUERY`
  共用同一份查询实现：进程双采样、IPC 采集、指标 ring 读取都带缓存与并发上限；
  `storage/tree` 也复用同一份 `rtstorage`，和服务管理共用一个阻塞 worker
  （整盘遍历较慢，但不占用进程/网络查询队列）
- 可能阻塞的接口（进程/网络/存储/诊断）用异步 handler 交给 `hv::async` 分发，
  真正的采集在共享 worker 上串行执行，不占 HTTP IO 线程
- 页面、`fs-ui.js`、`svc-ui.js`、`storage-ui.js`、`file-view.js`、`cm-editor.js` 在构建时
  压成 gzip 后以字节数组嵌入二进制，运行时直接发压缩体（非 gzip 客户端按需解压一次），
  不再在堆里保留副本
- 响应支持 gzip 协商（`Accept-Encoding: gzip`），HTML、sysinfo 与较大的 JSON 都会压缩，
  并带 `Vary: Accept-Encoding`
- sysinfo 快照在 loop 线程生成：页面打开（15s 内有请求）时 2s 一次，
  无人访问降到 10s 一次；页面发起请求时会立即补一次重建，首屏不等定时器
- 数据由 agent 主循环采集并缓存，HTTP 线程只读快照或调用带锁的共享查询，不直接访问隧道状态
- 两个构建变体：`rtunnel-agent`（带 Web）与 `rtunnel-agent-noweb`（纯隧道，体积更小）。
  体积敏感的场景选 lite 包，不要为了减体积再写一套精简页面；两边的页面、取数和组件源码只有一份。
  两者共用一个 `rtunnel-agent.service`，变体只由 `bin/rtunnel-agent` 软链指向哪个版本决定；
  带 Web 的构建默认监听 `127.0.0.1:8080`，可在 `rtunnel-agent.json` 用
  `"local_web": "0.0.0.0:8080"` 改地址，或用 `false` 关闭。
  `build/deploy/agent` / `build/deploy/agent-web` 是对应的两份部署目录；
  `deploy/package_agent.sh` 生成带 `install.sh` 的 `.tar.gz` 部署包（OTA 和手工安装共用），
  `deploy/publish.sh` 上传到文件仓库 `packages/agent/` 目录

#### 线程与内存

线程只在用到对应功能时创建（web 版本地页启动会预热查询），不随连接数增长。
`ps -L` 或 `/proc/<pid>/task/*/comm` 可直接看到 worker 名称：

| 线程 | 创建时机 | 作用 |
|---|---|---|
| 主事件循环 | 启动 | 隧道帧、定时器、所有 agent 状态 |
| 查询 worker（`rtagent-query`） | 首次进程/网络/诊断请求；web 版本地页启动预热 | 进程双采样、网络/IPC 快照、诊断包 |
| 阻塞 worker（`rtagent-work`） | 首次服务列表或展开存储分区 | systemctl/journalctl 与目录树扫描共用一个线程，服务请求优先；存储结果 60s 内复用 |
| HTTP IO | 启用 `--local-web` | 本地页与 WebSocket |
| `hv::async` | 首个异步接口请求 | 进程/网络/存储接口，上限 4、空闲 15s 退出 |

纯隧道、且没人打开设备详情页时只有主线程；打开详情页后按实际用到的功能增加，
页面轮询期间多一个异步分发线程，停止轮询 15s 后退出。扫描 worker 首次使用后
保留，避免每次展开分区都重建线程。服务列表和存储目录都不做定时重扫：页面打开时
查一次，之后只有手动「刷新 / 重新扫描」才触发。

扫描不并进查询 worker 是有意的：一次分区遍历上限 20s，`systemctl restart`
也可能几秒，而进程/网络页每 2s 自动刷新；共用一条队列会互相等。
按阻塞时长分三条队列，并且都按需启动，不用时不占线程。
RSS 里约 5–6 MB 是可回收的文件页（二进制、libc/libstdc++/libcrypto/libssl），
匿名内存主要来自本地页的进程预热缓存；`local_web: false` 时不预热，
空闲 RSS 与 `rtunnel-agent-noweb` 基本一致。需要更小常驻内存时用 noweb 构建
或关闭本地页。

### 共享前端组件

管理台与设备本地页的页面外壳是同一套（`DeviceView.vue`），文件、服务、存储三块
交互再共用 `shared/web/`，由两边各自挂载：

| 组件 | 作用 | 设备本地页 | 管理台 |
|---|---|---|---|
| `fs-ui.js` | 文件管理（WebSocket 会话、上传下载、编辑器） | 打包进本地页单文件 | `FileManagerCard.vue` 异步引入 |
| `svc-ui.js` | 服务管理（systemd 列表/日志/启停） | 打包进本地页单文件 | `SvcManagerCard.vue` 异步引入 |
| `storage-ui.js` | 存储分区折叠、目录瀑布、右键递归展开 | 打包进本地页单文件 | `StorageTreeCard.vue` 异步引入 |
| `file-view.js` | Hex / 文本 / 图片查看器 | 打包进本地页单文件 | 与 `fs-ui` 共用 |

组件都用 `createXxx(opts)` 挂到传入的容器上，样式自带作用域
（`fs-ui` 用 Shadow DOM，`storage-ui` 的规则全部挂在 `.rtstore` 下），
不依赖页面框架，也不污染宿主页面样式。改交互只改这一份。

### 文件管理（WebSocket）

设备详情页的「文件」tab 提供浏览、上传、下载、新建/重命名/移动/删除、
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
- 双击文件行在下方展开详情卡片（类型、大小、修改时间、完整路径，附打开/编辑/Hex/
  尾部/下载按钮），不再直接打开；打开改由卡片按钮、右键菜单或 `⋯` 菜单触发。
  双击目录行仍进入目录；操作列只保留下载/重命名/移动/删除，按可用宽度平铺，
  放不下的收进 `⋯` 菜单；信息行显示根目录、单文件上限和分区已用/总容量
- 重命名和移动在列表内展开表单（回车确认、Esc 取消），不弹浏览器对话框
- 文本编辑使用 CodeMirror 6（shell/json 语法高亮，Ctrl+S 保存、Esc 关闭），
  组件打包为 `shared/web/cm-editor.bundle.js`：管理台经 Vite 作为带 hash 的静态资源引入，
  本地页由 CMake 内嵌到二进制并从 `/assets/cm-editor.js` 加载
- JSON 文件在编辑器里点「树形」切换为树形查看：展开/收起、按需渲染（大文件不会卡）、
  解析失败给出提示；树形里点「源码」可切回编辑器，未保存内容一并带过去
- 路径隔离：`--file-root` 为唯一根，拒绝 `..`、绝对路径和逃出根目录的符号链接
  （Linux 5.6+ 用 `openat2(RESOLVE_BENEATH)`，MP135 5.15 可用）
- 管理台权限：`file.read` 只读，`file.write` 读写；agent 侧还会再按 `--file-ro/--file-rw`
  和 `--file-token` 校验一次
- 管理台「能力配置」可启停文件服务并修改根目录、读写模式、单文件上限，
  热生效并写入 agent 配置文件（`--config-file`，默认 `/opt/data/etc/rtunnel-agent.json`）
- relay-admin 通过本机 relay 打开管理流，使用进程内密钥认证（不落盘、不配置），
  操作者权限与设备范围在管理台侧校验（与 Web 终端一致）
- 本地 Web 与 relay 两条路径都可单独使用：lite 包没有本地页面，但管理台仍能管理文件

```bash
# 本地页面 + 局域网可写
./rtunnel-agent --devid board01 --server 192.168.1.133 --port 18443 \
    --file-root /opt/data/files --file-rw \
    --local-web 0.0.0.0:8080 --file-token <token>
```

### 文件页：文件仓库

导航「文件」页只有文件仓库（relay 本机的 `--admin-file-dir`）；设备文件仍然只在
设备详情页的「文件」tab 里。两个页面挂载的是**同一个** `shared/web/fs-ui.js`：
设备侧走 WebSocket，仓库侧给组件传一个 HTTP 后端（`relay/admin/web/src/shared/repo-backend.js`），
表格、工具栏、行内表单、右键菜单、编辑器、Hex/尾部查看器全部复用，只是仓库是扁平目录，
所以隐藏了路径栏、新建目录/文件和重命名/移动。

- 仓库表格与设备管理器完全一致：勾选列、名称/大小/修改时间（可排序）、
  搜索、底部汇总与批量操作；搜索按文件名过滤
- 行操作通过 `⋯`、右键或触屏长按 500ms 打开同一套菜单：部署到设备、分发到多台设备、
  下载、编辑（文本）、查看 Hex、查看尾部、复制 SHA-256、属性、删除
- 批量操作：下载（逐个）、打包下载（浏览器内生成 tar，支持 gzip）、删除；
  单行删除和批量删除都在页面内展开确认条，不使用弹窗
- 「部署」用行内表单：目标设备只列在线设备，默认沿用上次部署成功的设备；
  目标路径默认取该设备上次部署的目录 + 文件名，必须是绝对路径；
  `push_open` 由 relay 单独开一条 `OPEN_FS` 流，把仓库文件直接推给设备，
  浏览器只收 `push_progress/push_done`，不中转文件内容；目标已存在时默认不覆盖并
  标记冲突，勾选「覆盖同名文件」后重试。目标目录必须已存在
- 「分发到多台设备」跳到分发页（`/distribute?file=<name>`），沿用原有异步任务
- 仓库文本编辑走 `PUT /api/v1/files/<name>`（manager+，原子写，默认上限
  `file.max_edit_mb = 4` MiB）；保存后仓库哈希同步更新
- Hex / 尾部查看器是 `shared/web/file-view.js`，本地页、设备详情页和文件页共用：
  Hex 每页 4 KiB 按需读取（`download_open` 的 `offset/limit`），
  尾部读取最后 64 KiB、可选每 2 秒跟随；仓库侧用 HTTP `Range` 读取，
  设备侧用 WebSocket 范围下载，都不需要整文件下载
- 窄屏隐藏修改时间列，工具栏换行，行操作只保留「下载」和 `⋯`

### 服务管理（WebSocket）

设备详情页的「服务」tab 读取 systemd 服务列表，支持启动、停止、重启、
启用/禁用开机自启、屏蔽/解除屏蔽，并查看最近 200 行 journal 日志。
本地页面与管理台共用同一份前端（`shared/web/svc-ui.js`），relay 只转发，不解析协议：

```
本地页面  ws://device:8080/ws/svc
管理台    wss://relay/ws/svc/<devid>
              |
              v
        RTNP OPEN_SVC + DATA(u8 kind=0 + JSON)
              |
              v
        agent Service -> systemctl / journalctl
```

- 列表一次 `systemctl show '*.service'` 拿到已加载单元，再合并 `list-unit-files`，
  保证停止且未启用的单元也能列出来（否则无法从界面重新启用/屏蔽）；
  按「失败 → 启动/停止中 → 运行中 → 已停止」排序
- 状态行显示 PID 和已运行时长（由 `ActiveEnterTimestampMonotonic` + `/proc/uptime` 计算，
  不受设备时钟跳变影响）；单核设备上 `systemctl show` 约 1 秒，列表结果缓存 10 秒，
  动作后立即失效，正常轮询基本不花时间
- 表格分页与排序在 agent 完成，只回传当前页（默认每页 10 条，可选 5/10/20/50），
  名称、状态、自启、内存、运行时长五列均可点击排序，再次点击切换升/降序
- 搜索框按服务名/描述过滤，「全部/运行中/失败/已停止」标签显示计数；列表每 5 秒刷新，
  展开日志时每 2 秒刷新，页面不可见时暂停
- 停止、重启、禁用、屏蔽在行内展开确认条，不弹对话框；启停用 `systemctl --no-block`
  排队，界面随后刷新查看结果
- 启动/停止/重启/启用/禁用/屏蔽/取消屏蔽 全部动作在 `⋯` 菜单中按当前状态置灰；
  窄屏隐藏「自启/内存/运行时长」列，运行时长以紧凑形式显示在状态下方，
  分页只保留「当前页/总页数」和上一页/下一页
- 所有 `systemctl`/`journalctl` 调用在独立工作线程执行，不阻塞隧道和本地 Web 事件循环；
  `journalctl` 输出限制 256 KiB
- 默认只读：agent 加 `--svc-rw`（或配置 `svc_rw: true`）才允许控制；
  `--svc-token` 可给本地 Web 写操作加令牌，`--no-svc` 完全关闭（也不创建后台线程）。
  非 systemd 设备（无 `/run/systemd/system`）自动隐藏入口并上报 `caps.svc = false`
- 管理台「能力配置」可启停服务管理、切换只读/可控制，热生效并写入 agent 配置文件
- 管理台权限：需要 `device.svc` 才能操作服务；
  实际能否写仍由 agent 的 `--svc-rw`（或配置 `svc_rw: true`）决定

```bash
# 本地页面 + 服务控制
./rtunnel-agent --devid board01 --server 192.168.1.133 --port 18443 \
    --svc-rw --local-web 127.0.0.1:8080
```

## 三机部署示例

```bash
# B（192.168.1.133）：隧道 + 管理台
./rtunnel-relay --host 0.0.0.0 --port 18443 \
    --admin 0.0.0.0:9443 --admin-token <admin-token> \
    --admin-ca-cert ca.crt --admin-ca-key ca.key --admin-cert-dir certs \
    --admin-file-dir files

# C（192.168.1.55，板子）
./rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --allow-shell

# A（开发机）：rtk- 令牌来自管理台「接入账号」
./rtunnel-client --server 192.168.1.133 --port 18443 --token rtk-... \
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
| 客户端列表 Shell 没反应 | client 未加 `--allow-shell`，或 devid 不是 `a:<user>` |
| A 端上线即断开 | `--token` 缺失、不是 `rtk-` 令牌，或令牌停用/过期 |
| `OPEN_FAIL: acl denied` | 接入账号未加入权限组，或组里没有该设备/端口 |
| `OPEN_FAIL: shell disabled` | agent 未加 `--allow-shell` |
| client 启动报 `bind: Address already in use` | 默认会监听 1080，换 `--socks5 127.0.0.1:<port>` |
| 部署后 `undefined symbol: hv_version` | 用了共享库构建只拷了二进制；用 `--no-shared` 静态构建或一并部署 `libhv.so` |

## 实现说明

- **客户端认证**：只接受管理台签发的 `rtk-` 令牌，身份由令牌决定，`--user` 已移除；
  `atk-` 是管理台 API Token，不能用于数据面。令牌停用/过期/账号停用都会拒绝上线。
- **设备授权**：一律按权限组（资源 + 设备 + 端口）判断，未分组等于无权限；
  `--allow-all` 只用于本地测试。
- **Shell 授权**：`OPEN_SHELL` 需要权限组里的 `device.terminal` 且设备 agent 开 `--allow-shell`，
  两者缺一即 `OPEN_FAIL`。管理台 Web 终端固定使用内部代理身份。
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

