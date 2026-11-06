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

## 构建

```bash
./enp_build.sh --static --shared            # 产物在 build/bin/rtunnel-*
# 需要 TLS 时：
./enp_build.sh --static --shared --with-openssl
```

## 运行（明文）

```bash
# B
build/bin/rtunnel-relay --host 0.0.0.0 --port 8443 --acl alice='*' --allow-all
# C（设备侧，只允许本地 22 端口）
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22
# A（开发机）
build/bin/rtunnel-client --server B_IP --port 8443 --user alice --socks5 127.0.0.1:1080
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

## 测试

```bash
make rtunnel_test                  # 明文
RTUNNEL_TLS=1 make rtunnel_test    # TLS（需要 --with-openssl 构建）
RTUNNEL_MTLS=1 make rtunnel_test   # mTLS：证书 SAN 必须等于 devid（另含负向用例）
RTUNNEL_UDS=1 make rtunnel_test    # 本地入口走 UDS（需要 -DENABLE_UDS=ON 构建）
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

- 只读 API：`/api/v1/overview`、`/api/v1/agents`、`/api/v1/agents/{devid}`、
  `/api/v1/clients`、`/api/v1/streams`；
- 操作 API：`DELETE /api/v1/streams/{a_sid}`（踢会话）、`POST /api/v1/agents/{devid}/kick`
  （强制下线）、`GET/PUT /api/v1/acl/file`、`POST /api/v1/acl/reload`；
- 证书 API：`GET /api/v1/certs`、`POST /api/v1/certs/issue`（OpenSSL 签发，EC P-256）、
  `POST /api/v1/certs/sign`（上传 CSR 签名，私钥不出设备，身份取 SAN 缺省回退 CN）、
  `POST /api/v1/certs/revoke`（吊销即刻生效：踢在线连接 + 后续 AUTH 拒绝）；
- 其它：`GET /metrics`（Prometheus 文本指标）、`POST /api/v1/shutdown`（复用 drain 优雅退出）；
- 文件管理：`GET /api/v1/files?sort=name|size|mtime&order=asc|desc`（列表+排序）、
  `POST /api/v1/files/{name}`（raw body 上传，≤32MiB）、`GET /api/v1/files/{name}`（下载）、
  `DELETE /api/v1/files/{name}`（删除）；目录由 `--admin-file-dir D` 指定，
  文件名白名单防目录穿越，写入走 tmp+rename 原子替换；viewer 可读、operator 可写；
- 前端：`http://127.0.0.1:9443/`（侧边栏 + hash 路由；`admin.html/admin.js/admin.css`
  编译期内嵌，`--admin-docroot DIR` 可改为磁盘读取便于调试）；
- 鉴权与角色：`--admin-token T`（全权）或 `--admin-users F`（每行 `token:role`，
  role = viewer/operator/admin）；**非回环监听必须配置凭据**；
- 证书配置：`--admin-ca-cert C --admin-ca-key K --admin-cert-dir D`（台账 index.txt、
  黑名单 revoked.txt、审计 audit.log 均落在 D 下，启动自动加载黑名单）；
- 线程模型：HTTP 在 admin worker 线程，relay 状态访问经 `hloop_post_event` 投递回
  relay 线程执行（promise/future 同步），relay 数据无锁。

## 实现说明

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

