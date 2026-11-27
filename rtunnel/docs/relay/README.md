# relay（B）

公网入口：接收 agent 与 client 的连接、按权限组鉴权、分配会话、转发帧，同时提供管理台。
relay 本身不解析 SSH，也不存业务数据（除 SQLite 管理库与文件仓库）。

## 运行

```bash
build/bin/rtunnel-relay --host 0.0.0.0 --port 8443 \
    --admin 0.0.0.0:9443 --admin-token <首次初始化令牌> \
    --admin-db /var/lib/rtunnel/admin.db \
    --admin-file-dir /var/lib/rtunnel/files
```

第一次部署：浏览器打开管理台 → 输入 `--admin-token` + 管理员用户名/密码 → 创建 admin。
之后用的是账号密码（SQLite + HttpOnly 会话），token 只用于初始化，不再能登录。

非回环监听**必须**配 `--admin-token`，否则拒绝启动管理台。

## 参数

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
| `--admin-file-dir` / `--admin-docroot` | 文件仓库目录 / 前端磁盘目录（内嵌版可省略） |
| `--drain-timeout` | 优雅退出等待时长，默认 10000ms |
| `--stream-idle SEC` | 流空闲回收：超过 SEC 秒无帧就 RESET 并释放配额，默认 1800，0 关闭 |

relay 每 5s 巡检一次 agent 控制连接：连续 60s 没收到任何帧就断开并标记离线
（agent 每 15s 发 PING）。设备断电但 TCP 还挂着的“假在线”会在一分钟内收敛。

## TLS 与 mTLS

```bash
# relay
build/bin/rtunnel-relay --port 8443 --tls-cert server.crt --tls-key server.key
# agent / client
--tls-ca server.crt --tls-sni localhost
```

- mTLS：`--tls-ca` 让 B 要求客户端证书。C 侧校验证书 SAN 与 `AUTH.devid` 一致，
  A 侧校验 SAN 与 `AUTH.user` 一致——**身份以证书为准，不接受自报**。
- C 侧用 `--tls-cert/--tls-key` 提供设备证书；`--tls-sni` 填 B 证书的主机名。
- 管理台 HTTPS 与数据面 TLS 是两套参数，互不影响。自签证书首次访问需手动信任。

## UDS

`--socks5 unix:/path/to/socks.sock`（client 侧），仅在 `-DENABLE_UDS=ON` 构建时可用；
relay 自身的监听仍建议 TCP。

## 多 relay 与故障切换

A/C 都支持多个 relay 地址，按顺序尝试，连上后保持（sticky，不自动切回）：

```bash
# 重复 --server 或逗号分隔都行；host[:port] 用 --port 作默认端口
build/bin/rtunnel-agent  --server r1.example.com:8443,r2.example.com:8443 --devid dev1 --allow 22
build/bin/rtunnel-client --server r1.example.com --server r2.example.com --port 8443 \
    --token rtk-... --socks5 127.0.0.1:1080
# IPv6 用方括号：--server [2001:db8::1]:8443
```

- 最多 8 个地址，**顺序即优先级**：首次连接和每次重连都从高到低尝试，已建立的连接不被抢占。
- 连不上/握手超时的地址进 30s 冷却，冷却期内跳过；全部冷却时挑最早到期的探测。
- 退避 1s→30s，多地址时加 ±20% 抖动；AUTH_OK 后重置。
- 认证失败（token 错/过期/禁用/超额、证书身份不匹配）会把当前地址标记不可用，继续试下一个；
  全部被拒才停止重连（改完配置重启进程）。
- client 的按需 agent 继承完整地址列表；切到新 relay 时会停掉旧 relay 上的随需 agent。
- `shell <devid>` 在建立会话前也会依次尝试后续地址。

**管理域不共享**：每台 relay 的 `--admin-db` 是本机 SQLite，账号/权限/Token/审计/
仓库/任务各存一份。设备切到 r2 后 r1 看不到它是预期行为。详见
[`../architecture.md`](../architecture.md#多-relay-的边界)。

### 由 relay 下发地址列表

设备上线后，relay 可以把最新地址列表同步给 agent，agent 写入
`/opt/data/etc/rtunnel-agent.json`（`relay_servers` / `relay_servers_rev`），重启后继续用。

管理台入口在「设备与链路 → 中继列表」，地址维护和下发是两件事：

- **中继地址**：地址字典（host/port、名称、备注、启停），勾选后「加入下发列表」，
  ↑/↓ 调优先级（最多 8 个）。「保存列表」只更新期望状态。
- **设备**：勾目标设备，按需设开始时间、一次性/分批灰度、每批台数、批间隔、失败阈值、
  截止时间和最大重试，创建同步任务。
- 任务逐台下发并等 ACK：成功/失败/待上线分别记录；中间批次失败率超阈值会暂停，
  可继续、取消或重试（最后一批直接收敛，不再走闸门）。
- 创建任务时快照列表和 rev；之后改列表不改写已建任务；保存新列表会暂停快照过期的任务，
  恢复时要求 rev 一致，否则只能取消重建。
- 没有失败但仍有设备待上线的任务记为「部分完成」，不算成功。
- 中继同步任务只在中继列表页展示，不进「文件与升级 → 任务」中心。
- 每次 agent 认证成功，relay 自动下发当前列表，新设备/重启设备自动对齐。
- `--server` 里的地址不会被删除：启动时放在列表末尾兜底，列表写错也能救回设备。
- 新列表不含 agent 当前连接的 relay 时默认拒绝；确认要迁移再打开「允许迁移」。
- 接口：`/api/v1/relay-dict`（字典 CRUD/批量导入）、`/api/v1/relay-list`（有序列表 + ACK）、
  `/api/v1/relay-sync/tasks`（创建/详情/暂停/继续/取消/重试），权限 `relay.read` / `relay.write`。

## 运维

- **权限即时生效**：账号/权限组/令牌改动保存后立刻推给 relay，只影响新 OPEN，不动已建会话；
  接入账号一律按权限组鉴权，未分组等于无权限。
- **优雅退出**：`SIGTERM`/`SIGINT` 停止 accept，等现有连接收尾（`--drain-timeout`，默认 10s）
  后退出；第二次信号立即强退。A/C 断线后按既有退避重连。
- **流空闲回收**：每 5s 扫描，`--stream-idle` 秒内无帧则向两侧发
  `RESET(stream idle timeout)` 并释放会话配额；端口映射这类正常空闲业务请调大或设 0。
- **统计**：每 60s 一条 `[relay] stats:` 日志（agents/clients/streams/bytes/frames/auth 失败）。
- **用量**：流量（token）、会话次数/时长（token）、设备在线时长（device）按 5s 增量写入
  `usage_events` 并累加 `usage_daily`；`GET /api/v1/usage` 按日或总计查询（`usage.read`）。
  原始流水保留 180 天（`usage.raw_retention_days`），日聚合长期保留。管理台自身的终端/代理
  不计入客户用量。
- **带宽整形**：令牌可设 `rate_kbps`（kilobit/s，0 不限），令牌桶超额时暂停数据来源侧读
  （上行停客户端、下行停设备），靠 TCP 窗口回压，不丢帧；容量 200ms 的量（最小 8 KiB），
  单次暂停 ≤500ms。管理台在「接入账号 → Token」按 Mbps 填。
- **受控执行**：agent 配 `--allow-exec` 白名单后，`POST /api/v1/agents/:devid/exec` 可执行
  argv（不经 shell），stdout/stderr 分流、超时强杀、超字节截断；写审计 `device.exec`。
- **操作作业**：写动作走 `operations` 的 plan→apply→verify，`plan_hash` 防 TOCTOU、
  `Idempotency-Key` 防重放。risk ≥ 2 默认待审批，**必须由另一个账号批准**（自己批自己 403）；
  另有每分钟/每设备配额与 `HH:MM-HH:MM` 执行窗口。
- **事件流**：`GET /api/v1/events`（SSE）推上下线、会话、审计、告警、任务状态，带 `id`（seq）
  支持 `Last-Event-ID` 续传；受限账号只收自己设备的事件。
- **分钟级指标**：每分钟给在线设备要一份增量 sysinfo，落 `metrics_minute`（cpu/内存/负载/网卡），
  `GET /api/v1/agents/:devid/metrics/history?from=&to=&step=` 查趋势，默认留 7 天。
- **自身指标**：`GET /metrics` 输出 Prometheus 文本（在线设备/会话/流、连接与认证失败、
  收发包与字节、打开/失败/空闲回收的流、HTTP 派发耗时、SQLite 语句耗时与慢查询、告警队列、
  分发任务数）。需要 `device.view`，用 `atk-` Token 最方便：

  ```yaml
  scrape_configs:
    - job_name: rtunnel
      scheme: https
      authorization:
        credentials: atk-xxxxxxxx
      static_configs:
        - targets: ["relay.example.com:9443"]
  ```

  `/metrics` 是 relay 自己的指标；设备侧曲线在
  `GET /api/v1/agents/:devid/metrics/series`，两者不混用。
- **租户与计费**：设备与接入账号各属一个租户，数据面按租户强制隔离；停用租户等于停用其下
  全部接入账号并断开连接。套餐限制设备数、并发、月流量（0 不限）。价目与结算单见
  [`admin.md`](admin.md)。

## 线程模型

管理面 HTTP 默认 4 个 worker（`--admin-threads`），relay 状态访问经 `hloop_post_event`
投递回 relay 线程执行（promise/future 同步），数据面无锁。

`/device` 的 HTTP 代理每个请求一条独立线程（上限 `proxy.max_concurrent`）；终端/文件/服务
WebSocket 各有自己的读线程，管理面 worker 不会被长连接或慢设备拖住。分发/告警/清理等后台
任务由 `AdminServer` 统一管理，`stop()` 时置标志、收掉代理线程并 join 全部 worker。
