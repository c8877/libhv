# 架构与实现

## 拓扑

```
开发者 ssh ──本地 SOCKS5/端口映射──> A ══TCP/TLS══> B ══TCP/TLS══> C ──> 127.0.0.1:22
                                    client            relay           agent
```

三段里只有 B 需要可访问地址。C 主动连 B 并注册 `devid`；A 主动连 B，用 `rtk-` 令牌
换到「能连哪些 `devid:port`」的授权；A 要连设备时向 B 发 OPEN，B 按 `c_sid` 找到 C 的
连接把帧转过去。整条链路上 B 不解析 SSH，只搬字节。

选择反向通道而不是端口映射/内网穿透的原因、被否掉的方案，见
[`../../docs/design/rtunnel.md`](../../docs/design/rtunnel.md)。

## 模块划分

```
rtunnel/
├── rtnp/       协议库：Frame / Decoder / Writer / Reader（纯 C++11，无依赖）
├── common/     共享组件：TunnelLink（长连接 HELLO/AUTH 状态机、心跳、退避重连）
├── relay/      relay 主程序 + admin/（管理台：HTTP、SQLite、AI、部署）
├── agent/      agent 主程序 + web/（内嵌设备本地页，构建产物）
├── client/     client 主程序
├── shared/     两端共用的 C++ 小工具
├── deploy/     打包、发布、自举、MCP 桥、测试环境部署手册
├── tests/      e2e 脚本（Python/shell）与 tests/ui（Playwright）
└── docs/       本文档
```

约定：

- **不改 libhv**：所有能力都在 `rtunnel/` 内实现，只使用 libhv 的公开 API
  （`hloop_post_event`、`hio_get_ssl`、`hv::async` 等）。
- **对象化**：状态收进 `App`/`Server` 类成员，libhv 回调经 `hio_context` /
  `hevent_userdata` 取回对象；Buffer/Writer 等用 RAII 管理。
- **协议只有一份**：帧编解码在 `rtnp/`，任何一个端改字段都要同时改
  `tests/protocol_doc_test.py` 覆盖的协议文档。

## 协议要点

| 项 | 值 |
|---|---|
| 帧头 | 长度 + 类型 + sid + flags，帧类型见设计文档 §协议 |
| 握手 | `HELLO`（5s 超时）→ `AUTH`（10s）→ `AUTH_OK`；未认证连接 10s 断开 |
| 心跳 | 两端 15s 一次 `PING`，连续 3 个没有 `PONG` 判死重连 |
| 打洞 | `OPEN`（15s 超时）→ `OPEN_OK` / `OPEN_FAIL`（带原因码） |
| 关闭 | 半关闭后 60s 未对关发 `RESET(CLOSE_TIMEOUT)` |
| 空闲 | relay 每 5s 扫描，`--stream-idle` 秒内无帧则 `RESET(stream idle timeout)` |
| 版本 | `HELLO` 带版本区间；低版本拒绝，高版本向前兼容（`compat_e2e.py`） |

流控：发送方受 credit 限制；接收方在本地写队列降到 `RTNP_LOW_WATER` 后才回 `WINDOW`，
避免慢端把快端的内存吃满。

## 认证与授权

- 数据面只认 `rtk-` 令牌，身份由令牌决定（`--user` 已移除）。`atk-` 是管理台 API Token，
  不能用于数据面。
- 令牌停用/过期/归属账号停用都会在下次认证时拒绝上线。
- 授权一律按权限组：资源键 + 设备范围 + 端口范围。`--allow-all` 只用于本地测试。
- mTLS 打开后身份以证书为准：C 侧校验证书 SAN 与 `AUTH.devid` 一致，A 侧校验 SAN 与
  `AUTH.user` 一致，不接受自报。

## 多 relay 的边界

A/C 支持最多 8 个 relay 地址，顺序即优先级，按序尝试并保持 sticky（不自动切回）。
连不上/握手超时的地址进 30s 冷却，退避 1s→30s 带 ±20% 抖动。

但**每个 relay 是独立管理域**：`--admin-db` 指向本机 SQLite，账号、权限组、Token、
审计、证书台账、文件仓库、分发/OTA 任务各存一份，互不同步。

| 数据 | 多 relay 之间 |
|---|---|
| 设备在线状态 / 隧道 | 不共享；设备同一时刻只连一个 relay |
| 管理台账号、会话、权限组 | 不共享；换 relay 要重新登录，账号可能不存在 |
| API Token、接入令牌 | 不共享；r1 签发的 token 在 r2 无效 |
| 审计、证书台账、分发/OTA 任务 | 不共享 |
| agent 本地采样配置 | 跟设备走；relay 侧的任务和权限记录不跟 |

因此：设备切到 r2 后 r1 看不到它是预期行为；不要把两个 relay 指向同一个 SQLite 文件
（锁不跨主机，会损坏库）；负载均衡不要把管理请求轮询到多个 relay。需要单一入口时，
只让一个 relay 承担管理，其余做纯链路备份。

## 目录与部署形态

| 组件 | 安装前缀 | 配置 | 二进制 |
|---|---|---|---|
| relay | `/opt/rtunnel/relay` | `/etc/rtunnel/relay.env` | `bin/rtunnel-relay`（版本化软链） |
| agent | `/opt/rtunnel/agent` | `/opt/data/etc/rtunnel-agent.args` + `.json` | `bin/rtunnel-agent` |
| client | `/opt/rtunnel/client` | `/etc/rtunnel/client.env` | `bin/rtunnel-client` |

自举只需要 `rtunnel-client`：`rt-bootstrap.sh` 取 `packages/<组件>/latest/<arch>.json`
→ 下载 → 校验 sha256 → 解包 → `install.sh`。细节见 [`dev.md`](dev.md)。

## 已知限制

- libhv 在本地 socket 读到 EOF 时关闭 io，因此「SOCKS 客户端先 FIN、A 再回写剩余数据」
  不支持；SSH 正常关闭（设备端先关）不受影响。
- 设备 Web 代理不支持设备侧 HTTPS；路径式改写对部分 SPA 仍可能缺资源。
- SQLite 单机，不支持共享库/主从；多 relay 不做管理高可用。
- 一个设备同一时刻只连一个 relay，切换期间会话会断。
