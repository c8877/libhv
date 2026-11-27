# client（A）

开发者侧的进程。连 relay，用 `rtk-` 令牌认证，在本地提供 SOCKS5 或端口映射；
也可以直接开一个到设备的 shell，或读写管理台文件仓库。

## 运行

```bash
# SOCKS5（最常用）
build/bin/rtunnel-client --server 192.168.1.133 --port 8443 --token rtk-... \
    --socks5 127.0.0.1:1080

# 端口映射（不需要 SOCKS5 客户端）
build/bin/rtunnel-client --server 192.168.1.133 --port 8443 --token rtk-... \
    --forward 127.0.0.1:16022=board01:22

# 只开 shell，不占本地端口
build/bin/rtunnel-client --server 192.168.1.133 --port 8443 --token rtk-... \
    shell board01

# 读写文件仓库（HTTPS + atk-，不走隧道）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
    build/bin/rtunnel-client repo ls /packages/agent
```

`--socks5` **可以省略**：省略时只做 `--forward` / `shell`，不会再默认占用 1080。

SOCKS5 方式的 `~/.ssh/config`：

```sshconfig
Host PILE-000123
    HostName PILE-000123
    User root
    ProxyCommand nc -X 5 -x 127.0.0.1:1080 %h %p
    HostKeyAlias %h
```

端口映射方式：

```sshconfig
Host pile
    HostName 127.0.0.1
    Port 16022
    User root
    HostKeyAlias PILE-000123
```

## 参数

| 参数 | 说明 |
|---|---|
| `--server/--port --token` | relay 地址与 `rtk-` 令牌（身份由令牌决定）；`--server` 可重复或逗号分隔（最多 8 个，自动切换） |
| `--socks5 HOST:PORT` / `unix:PATH` | 本地 SOCKS5 入口；可省略 |
| `--forward 127.0.0.1:16022=devid:22` | 本地端口映射，可多次 |
| `--allow-shell` | 允许 relay 通知本机按需拉起 agent（默认关） |
| `--agent-path` / `--agent-idle` | agent 路径 / 空闲自退秒数（默认 15） |
| `--tls-ca/--tls-cert/--tls-key/--tls-sni` | TLS / mTLS |
| `shell <devid>` | 本机终端直连设备免密 shell |
| `repo <mkdir\|put\|get\|stat\|ls\|rm>` | 直接读写文件仓库（HTTPS + atk-）；`put/get` 支持断点续传，`get` 落盘后校验 sha256 |
| `--version` | 版本号（打包/自举/安装自检都用它） |

## 按需 shell（管理台「客户端」列表的 Shell 按钮）

client 加 `--allow-shell`（默认关）后：

1. 管理台「客户端」点 Shell，admin 对 `a:<user>` 发 `OPEN_SHELL`；
2. relay 发现 `a:<user>` 未注册且该 user 的 client 在线，下发 `SPAWN_AGENT`，挂起 OPEN（10s 超时）；
3. client 在本机 `fork/exec` 同目录的 `rtunnel-agent`（`--agent-path` 可指定），
   以 `a:<user>` 注册；relay 把挂起的 OPEN 转给这个 agent；
4. 无会话空闲 `--agent-idle` 秒（默认 15）后 agent 自退，client 退出时也会回收。

client 只做“拉起/回收”，shell 逻辑完全复用 agent；relay 只多一个挂起状态和新控制帧，
不在 A-B 连接上叠双角色。

## 多 relay

```bash
build/bin/rtunnel-client --server r1.example.com --server r2.example.com --port 8443 \
    --token rtk-... --socks5 127.0.0.1:1080
```

顺序即优先级，连上后保持（sticky）。按需 agent 继承完整地址列表；切到新 relay 时会停掉
旧 relay 上的随需 agent。详见 [`../relay/README.md`](../relay/README.md#多-relay-与故障切换)。

## 注意

- 令牌必须是 `rtk-`（`atk-` 不能用于数据面）；令牌停用/过期/归属账号停用都会拒绝上线。
- 权限来自权限组：没有该设备或该端口的授权会收到 `OPEN_FAIL: acl denied`。
- 本地 1080 被占时报 `bind: Address already in use`，换端口或省略 `--socks5`。
- 构建如果是共享库版本，部署时别忘了 `libhv.so`，否则报 `undefined symbol: hv_version`。
