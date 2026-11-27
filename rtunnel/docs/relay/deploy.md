# 从管理台部署到内网主机

「接入与自动化 → 部署」是设备接入前的入口：登记一台能 SSH 登录的主机，平台把 agent /
relay / client 装上去，不用先手工 scp。测试环境改用直推（不经管理台）见
[`../../deploy/DEPLOY.md`](../../deploy/DEPLOY.md)。

## 流程

```
扫描网段（可选）→ 登记主机 → 测试连接 → 选安装包 → 填参数 → 开始部署
      ↓                                         ↓
   nmap / TCP                          预检 → 传包 → 解包 → 写配置
                                                  → install.sh → 验证 → 清理
```

## 扫描网段

不知道要装哪台机器时，填一个私有网段（如 `192.168.1.0/24`，最多 1024 个地址）。

- relay 主机上有 nmap 时用 `nmap -Pn -n -p<port> --open -T4 --max-retries 1 --host-timeout 4s`。
  `-Pn` 让屏蔽 ICMP 的主机也不漏，`-n` 不反解 DNS。
- 没有 nmap 的精简/静态镜像退回内置非阻塞 TCP 连接扫描（128 并发，读 `SSH-` 横幅判服务）。
  两者结果结构一致，`engine` 字段标明用的哪条。
- 只接受私有网段（10/8、172.16/12、192.168/16、127/8、169.254/16、100.64/10），
  单次最多 1024 个地址；`engine=nmap` 时没装 nmap 直接报错，不悄悄换引擎。
- 结果与已登记的 `host:port` 比对，回 `known`/`host_id`；页面上已登记的直接「去部署」。
- **扫到不等于能登录**：扫描只探端口，SSH 协议能不能谈成要看「测试连接」。

接口：`POST /api/v1/deploy/scan`，`{cidr, port, timeout_ms, engine}`，权限 `deploy.read`。

## 主机与凭据

- 登记：地址 + 端口 + 登录用户 + 密码或私钥 + 是否 sudo（非 root 账号）。
- 凭据只存 relay 本机 `<admin.db 同目录>/deploy/<host_id>.secret|.key`（0600），
  库里只有 `secret_set` 标记，接口从不回显。
- 主机密钥 **accept-new**：首次连接把 `sha256:<base64>` 记进 `deploy/known_hosts`，
  之后指纹不一致直接拒绝（防中间人）。确认是重装/换机后，删掉对应行再试。
- 测试连接返回：架构、是否 root、有没有 systemd、`/tmp` 剩余空间、已装版本。

SSH/SFTP 由内置 libssh2 完成（`3rd/libssh2`，OpenSSL 后端，裁剪源码）：

- relay 主机不需要装 ssh 客户端、scp 或 sshpass；目标机不需要预装任何东西。
- 密码认证不走 tty 技巧；sudo 密码直接写进通道 stdin（`sudo -S`）。
- 私钥支持 RSA(PEM) 与 ed25519(openssh-key-v1)。
- 等待按**总超时**判断（握手/认证 20s），不会因为服务端慢几百毫秒发 banner 就失败。

## 安装包

直接用文件仓库 `packages/<组件>/<版本>/<架构>/` 下的包，和 OTA 分发同一份。

- 文件名约定 `rtunnel-<component>[-<variant>]-<version>-<arch>.tar.gz`，
  列表接口递归扫 `packages/` 并解析组件/版本/架构，返回 `path`。
- 创建作业时 `package` 传这个 `path`（也接受文件名）；只认列表里存在的包，
  仓库外路径和 `..` 一律拒绝。
- 页面上的「上传安装包」按包名自动建 `packages/<组件>/<版本>/<架构>/` 再落盘，
  上传后 OTA 立刻可见。

## 安装步骤

1. **预检**：连通性、`uname -m` 与包架构比对、是否 root、有没有 systemd、`/tmp` 空间。
   装到 `/opt/rtunnel/*` 并接管服务时必须 root 或勾 sudo；自定义前缀 + 只装文件可不用。
2. **上传**：SFTP 到 `/tmp/rtunnel-deploy-<id>`。
3. **解包**：`tar --strip-components=1`。
4. **写配置**（在 `install.sh --start` 之前）：
   - agent：`/opt/data/etc/rtunnel-agent.args`（`--devid/--server/--allow`）
   - relay：`/etc/rtunnel/relay.env`（`RELAY_ARGS`，含 `--admin` 与初始令牌）
   - client：`/etc/rtunnel/client.env`（relay 地址）
   - 自定义前缀时配置目录跟随为 `<prefix>/etc`
5. **执行**包内 `install.sh`（`--no-units` 时只装文件，不 enable/start）。
6. **验证**：版本、`systemctl is-active`、relay 额外探 `https://127.0.0.1:<admin>/`；
   只装文件时只比对版本。
7. **清理**临时目录。

## 参数与约束

| 组件 | 必填 | 说明 |
|---|---|---|
| agent | 设备 ID、relay 地址 `host:port` | 地址是数据面端口，不是管理台；端口白名单留空表示不转发 |
| relay | 管理端口（默认 9443）、初始令牌 | 令牌留空自动生成并写进作业日志 |
| client | relay 地址 | 令牌在管理台签发后写到目标机 |

- 主机/用户名/设备 ID 限 `[A-Za-z0-9._-]`；安装目录必须绝对路径且不含 `..`；
  端口白名单只允许数字和逗号；远端 shell 串一律单引号包裹，避免命令注入。
- 一台主机同时只允许一个未结束作业；日志实时追加（尾部 128KB），可取消。
- 取消只置标志，SSH 读循环看到后断开通道。

接口：`/api/v1/deploy/hosts`（CRUD + `/probe`）、`/api/v1/deploy/packages`、
`/api/v1/deploy/jobs`（创建/列表/详情/取消），权限 `deploy.read` / `deploy.write`。

## 失败处理

见 [`../troubleshooting.md`](../troubleshooting.md#部署)。最常见的三类：架构不匹配、
非 root 且没 sudo、容器里没有 systemd（应勾「只装文件」）。
