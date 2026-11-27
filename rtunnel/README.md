# rtunnel：A → B → C 远程 SSH 隧道

```
开发者 ssh ──本地 SOCKS5──> A ══TCP/TLS══> B ══TCP/TLS══> C ──> 127.0.0.1:22
```

| 程序 | 角色 |
|---|---|
| `rtunnel-relay` | B：接入、devid 注册表、权限组、c_sid 分配、帧转发、管理台 |
| `rtunnel-agent` | C：连 B、上报 devid、执行 OPEN、透传字节、可选本地页 |
| `rtunnel-client` | A：连 B、本地 SOCKS5 / 端口映射入口、按需拉起 agent |

三个组件只需要一个 `rtunnel-client` 就能自举安装和升级（含它自己），目标机不用装
curl/scp/ssh。

## 最短路径

```bash
# 构建（仓库根目录）
./enp_build.sh --static --shared --with-openssl

# B
build/bin/rtunnel-relay --host 0.0.0.0 --port 8443 \
    --admin 0.0.0.0:9443 --admin-token <首次初始化令牌> --admin-db /var/lib/rtunnel/admin.db
# C
build/bin/rtunnel-agent --server B_IP --port 8443 --devid PILE-000123 --allow 22
# A（rtk- 令牌在管理台「组织与权限 → 接入账号」签发）
build/bin/rtunnel-client --server B_IP --port 8443 --token rtk-... --socks5 127.0.0.1:1080
```

浏览器打开 `http://B:9443/`，用 `--admin-token` 初始化管理员。之后加设备、发令牌、
配权限都在管理台里做（未分组的账号等于无权限）。

## 文档

全部文档在 [`docs/`](docs/README.md)，按“先看什么”排：

| 想知道 | 看 |
|---|---|
| 各角色怎么跑、参数是什么 | [`docs/README.md`](docs/README.md) → 对应组件 |
| 协议、模块划分、超时与流控 | [`docs/architecture.md`](docs/architecture.md) |
| 管理台/设备页怎么设计、怎么改 | [`docs/uiue.md`](docs/uiue.md) |
| relay 运行、TLS、多 relay、运维 | [`docs/relay/README.md`](docs/relay/README.md) |
| 管理台功能（权限、文件、OTA、AI） | [`docs/relay/admin.md`](docs/relay/admin.md) |
| 从管理台把组件装到内网主机 | [`docs/relay/deploy.md`](docs/relay/deploy.md) |
| A 端（client）用法 | [`docs/client/README.md`](docs/client/README.md) |
| C 端（agent）能力、本地页 | [`docs/agent/README.md`](docs/agent/README.md) |
| agent OTA 升级与回滚 | [`docs/agent/ota.md`](docs/agent/ota.md) |
| 构建、测试、打包发布 | [`docs/dev.md`](docs/dev.md) |
| 连不上 / 打不开 | [`docs/troubleshooting.md`](docs/troubleshooting.md) |

设计文档（反向通道的推演、状态机、协议表）仍在
[`../docs/design/rtunnel.md`](../docs/design/rtunnel.md)；测试环境的直推步骤在
[`deploy/DEPLOY.md`](deploy/DEPLOY.md)。
