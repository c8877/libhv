# rtunnel：A → B → C 远程 SSH 隧道

设计见 [`docs/design/remote-ssh-tunnel.md`](../docs/design/remote-ssh-tunnel.md)。

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

开发者 `~/.ssh/config`：

```sshconfig
Host PILE-000123
    HostName PILE-000123
    User root
    ProxyCommand nc -X 5 -x 127.0.0.1:1080 %h %p
    HostKeyAlias %h
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
```

覆盖：CONNECT 建链、数据回环、离线设备、端口白名单、ACL、并发（两个 A 客户端同 sid 不串流）、
设备端关闭传播、256 KiB 大流量；另有 `build/bin/rtnp-codec-test` 校验帧编解码。

## 与设计文档的差距（v1 实现）

- **设备身份**：当前 B 接受 C 自报的 devid，未做 mTLS 的 SAN 绑定；生产环境需补证书校验。
- **本地入口**：只实现 `127.0.0.1:PORT` 的 SOCKS5，设计里的 UDS 默认入口未接。
- **半关闭**：隧道层支持 `CLOSE` 半关闭，但 libhv 在本地 socket 读到 EOF 时会直接关闭 io，
  因此「SOCKS 客户端先 FIN、再由 A 回写剩余数据」这条路走不通；SSH 正常关闭不影响。
- 未实现流控的「按写队列低水位补窗口」，当前收到 DATA 写入本地后即回 `WINDOW`
  （仍在 credit 上限内，不会无界增长）。
