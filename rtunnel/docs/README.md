# rtunnel 文档

一台设备在 NAT 后面、没有公网 IP、也不允许装 SSH 服务端时，怎么让开发者直接 `ssh` 上去。
rtunnel 用 A（开发者机）→ B（有公网的 relay）→ C（设备）三段反向隧道解决这件事，
并且在 B 上带一个完整的管理台。

## 文档地图

| 文档 | 内容 | 谁看 |
|---|---|---|
| [`architecture.md`](architecture.md) | 拓扑、协议帧、模块划分、超时/流控、实现约束 | 要改代码的人 |
| [`uiue.md`](uiue.md) | 管理台与设备页的布局、页面范式、组件规约、移动端、文案 | 要改页面的人 |
| [`dev.md`](dev.md) | 构建、测试、打包、发布、自举 | 开发/发版 |
| [`troubleshooting.md`](troubleshooting.md) | 连不上、打不开、权限拒绝的排查表 | 现场 |
| [`relay/README.md`](relay/README.md) | relay 运行、TLS/mTLS、多 relay、下发列表、运维 | 部署 B 的人 |
| [`relay/admin.md`](relay/admin.md) | 管理台：权限、Token、设备页、文件/分发/OTA、AI、Web 终端 | 用管理台的人 |
| [`relay/deploy.md`](relay/deploy.md) | 从管理台 SSH 到内网主机装组件、扫网段 | 运维 |
| [`client/README.md`](client/README.md) | A 端：SOCKS5、端口映射、按需 shell、repo 子命令 | 开发者 |
| [`agent/README.md`](agent/README.md) | C 端：能力开关、系统信息、本地页、文件/服务管理 | 设备侧 |
| [`agent/ota.md`](agent/ota.md) | OTA 流程、状态机、失败回滚 | 发版/现场 |

设计推演（反向通道的取舍、协议表、状态机）在
[`../../docs/design/rtunnel.md`](../../docs/design/rtunnel.md)；测试环境直推在
[`../deploy/DEPLOY.md`](../deploy/DEPLOY.md)。

## 5 分钟跑通（明文）

```bash
# 仓库根目录构建；要 TLS 加 --with-openssl
./enp_build.sh --static --shared

# B：192.168.1.133
./build/bin/rtunnel-relay --host 0.0.0.0 --port 18443 \
    --admin 0.0.0.0:9443 --admin-token <admin-token> \
    --admin-ca-cert ca.crt --admin-ca-key ca.key --admin-cert-dir certs \
    --admin-file-dir files

# C：板子 192.168.1.55
./build/bin/rtunnel-agent --server 192.168.1.133 --port 18443 --devid board01 \
    --allow 22 --allow-shell

# A：开发机（rtk- 令牌来自管理台「组织与权限 → 接入账号」）
./build/bin/rtunnel-client --server 192.168.1.133 --port 18443 --token rtk-... \
    --socks5 127.0.0.1:11080 --forward 127.0.0.1:16022=board01:22 \
    --allow-shell --agent-idle 120
```

```bash
ssh -p 16022 root@127.0.0.1
```

管理台：`http://B:9443/`，首次用 `--admin-token` 初始化管理员，之后是账号密码登录。
没有 `rtk-` 令牌时 A 会在认证阶段断开，这是预期行为（身份由令牌决定，不看 `--user`）。

也可以走 SOCKS5（不需要端口映射）：

```sshconfig
Host PILE-000123
    HostName PILE-000123
    User root
    ProxyCommand nc -X 5 -x 127.0.0.1:1080 %h %p
    HostKeyAlias %h
```

## 默认端口与文件位置

| 项 | 默认 | 备注 |
|---|---|---|
| relay 数据面 | `0.0.0.0:8443` | 部署包里常用 18443 |
| 管理台 | 不开，需 `--admin ADDR` | 常用 `:9443`，可自签 HTTPS |
| A 端 SOCKS5 | `127.0.0.1:1080` | `--socks5` 可省略，省略后不再占 1080 |
| agent 本地页 | `127.0.0.1:8080` | `--local-web` 开启，`false` 关闭 |
| relay 安装前缀 | `/opt/rtunnel/relay` | 配置 `PREFIX/etc/relay.env` |
| agent 安装前缀 | `/opt/rtunnel/agent` | 配置 `/opt/data/etc/rtunnel-agent.{args,json}` |
| client 安装前缀 | `/opt/rtunnel/client` | 配置 `/etc/rtunnel/client.env` |
| 管理数据 | `--admin-db`（SQLite） | 部署包放 `PREFIX/var/admin.db` |
| 文件仓库 | `--admin-file-dir` | 部署包放 `PREFIX/var/files` |

设备侧一切可变文件都在 `/opt/data`（数据分区，重启/升级保留）；
relay 自己的配置在 `/etc/rtunnel/relay.env`。

## 术语

- **relay / B**：公网入口，负责注册、鉴权、配额、转发。
- **agent / C**：设备侧常驻进程，只做出站连接，不需要公网 IP。
- **client / A**：开发者侧进程，提供本地 SOCKS5 或端口映射。
- **devid**：设备 ID，agent 启动参数；也是 SSH 里的主机别名。
- **`a:<user>`**：client 按需拉起的临时 agent 的身份，用于「客户端列表 → Shell」。
- **rtk-**：接入令牌，给 A/设备用，身份由令牌决定。
- **atk-**：管理台 API Token，给脚本/CI 用；不能用于数据面。
- **c_sid / a_sid**：relay 分配的会话 ID（设备侧 / 客户端侧），只在日志与协议里出现，
  界面不展示这两个字母。

## 约定

- 组件之间只有 relay 主动监听；agent 和 client 都是出站连接，能过绝大多数 NAT/防火墙。
- 权限一律由权限组求值：资源 + 设备范围 + 端口。未分组等于无权限，没有“按角色回落”。
- 管理台页面只调 `/api/v1/*`，数据面（隧道）走 RTNP 帧，两者不复用端口。
- 开发阶段不写兼容层：旧目录布局、旧配置格式直接重装，不做自动迁移。
