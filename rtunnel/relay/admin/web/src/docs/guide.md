这份指南按「装起来 → 接设备 → 分权限 → 日常运维 → 接 AI」的顺序走。步骤里提到的页面都可以直接点开，
命令可以直接复制。

## 1. 这套系统是什么 {#arch}

rtunnel 是一套**反向隧道 + 远程运维**平台：被管设备主动连到一台有公网入口的 relay，
你在管理台里通过 relay 访问设备，不需要设备有公网 IP，也不需要端口映射。

- **relay**：公网/中心侧。跑隧道接入 + 管理台（HTTP API、WebSocket 终端、前端页面）。
- **agent**：装在设备上。主动连 relay，提供端口转发、文件、服务、串口、诊断、受控执行。
- **client**：装在运维人员机器上。把设备端口映到本地、开 SOCKS，也可以直接读写文件仓库。
- **admin**：relay 自带的管理台，浏览器打开 `https://relay:9443`。

> 设备、账号、权限组、令牌、审计、任务都存 relay 的本地 SQLite；每台 relay 是独立的。
> 多台 relay 目前只做链路冗余，不做管理面共享。

## 2. 部署 relay {#deploy}

### 方式 A：单二进制（推荐）

前端已经编进二进制，不需要另外分发 `admin/web`，避免“换了 relay 忘了换前端”。

```bash
cmake -S . -B build_static -DBUILD_SHARED=OFF -DRTUNNEL_ADMIN_WEB=ON
cmake --build build_static --target rtunnel-relay-embedded -j8
# 产物 build_static/bin/rtunnel-relay-embedded，落到目标机后改名/软链为 rtunnel-relay
```

### 方式 B：部署包 + 自举

包名统一是 `rtunnel-<component>-<ver>-<arch>.tar.gz`，里面带 `install.sh`、
`bin/rtunnel-client` 与 `bin/rt-bootstrap.sh`。

```bash
# 目标机只要先有一个 rtunnel-client
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rt-bootstrap.sh --component relay --version latest --arch x86_64 --start

# 无 systemd 环境
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
```

> `install.sh` 只做文件操作：先校验架构、`--version` 与 `SHA256SUMS`，再原子切换软链。
> `--start` / `--restart` 才会动 systemd；重启后健康检查失败会自动回滚一次。

参数与证书相关的开关在 [系统设置](#/settings) 里能看到当前值。

## 3. 接入设备 {#device}

设备上装 agent，启动时指定 relay 地址与自己的 devid。agent 会主动连上来，
之后在「设备」页就能看到它。

```bash
# 最小启动
rtunnel-agent --server relay.example.com --port 18443 --devid box-001

# 常见附加参数
--allow 22,80,443          # 允许转发的端口白名单
--file-dir /opt/data/files # 文件管理根目录
--allow-shell              # 允许管理台开 shell
--allow-tty /dev/ttyS0     # 允许访问的串口
--allow-exec "uptime;df -h;systemctl status *"   # 受控命令白名单（不配则 exec 不可用）
```

### 不用命令行也能装：运维 → 部署

如果这台主机还能 SSH 登录，可以交给「运维 → 部署」直接装，不用先手工传包：

1. 「主机」页签登记地址、端口、登录用户和密码/私钥（非 root 勾上 sudo）。凭据只存在 relay
   本机，页面不会回显；选中主机会自动测一次连通性，给出架构、systemd、`/tmp` 剩余空间。
2. 安装包放到仓库 `deploy/` 目录，页面也能直接上传。文件名按
   `rtunnel-<组件>[-变体]-<版本>-<架构>.tar.gz` 放，列表会自动识别。
3. 「新建部署」选组件（agent / relay / client）和包；agent 要填设备 ID、relay 地址
   （数据面 `host:port`）和端口白名单，relay 填管理端口和初始令牌（留空自动生成）。
4. 点「开始部署」，后台会预检 → 传包 → 解压 → 写配置 → 执行包内 `install.sh` → 验证服务和版本。
   日志实时追加，可取消；装完设备就会出现在「设备」列表里。

> SSH/SFTP 用的是 relay 内置的 libssh2，目标机不需要装 ssh 客户端、sshpass 或任何 agent。
> 容器/临时目录里可以勾「只安装文件，不接管 systemd」。

### 批量升级

文件仓库上传包后，在「OTA」页选包、按架构过滤设备、灰度下发。失败会按失败率闸门停止，
agent 侧有 boot-check 自动回滚。

- [OTA 升级](#/ota)
- [文件分发](#/distribute)

> agent 只连 relay，不扫描内网、不做设备发现；也不接受 relay 让它去连别的机器。

## 4. 账号、权限组与令牌 {#perm}

权限**只来自权限组**：账号角色本身不授予任何权限，只决定账号管理页能管谁。
一个账号或令牌能做什么，取决于它所在组勾了哪些资源键，以及组的设备范围。

- **权限组**：勾资源键（`device.view`、`file.write`、`device.exec`…）+ 设备范围（全部设备或指定设备）+ 端口范围。
- **授权对象**：把组绑给控制台账号、接入账号或某个令牌；令牌级分组只认自己绑的组。
- **接入账号 / 令牌**：给外部系统用的 `rtk-` 令牌，可设时效、流量、并发、带宽。
- **租户**：设备与接入账号的归属边界，令牌只能看到、连到本租户设备；停用租户 = 停用其下全部账号。

新环境的最小路径：建权限组（勾所需资源 + 设备范围）→ 建账号 → 在「授权对象」里把组绑给账号 → 签发令牌。

| 页面 | 用途 |
|---|---|
| [权限组](#/groups) | 定义“能做什么 + 对哪些设备” |
| [授权对象](#/subjects) | 把组绑给账号或令牌 |
| [接入账号](#/access) | 外部系统的账号与令牌 |
| [租户](#/tenants) | 客户/设备的归属边界 |

> 令牌不继承运营商侧资源（权限组、字典、中继、证书、设置、审批），即使被误授也不生效。
> 设备类接口只返回授权范围内的设备。

## 5. 日常运维 {#daily}

- **看状态**：「设备」页有 sysinfo、负载、内存、磁盘、网卡、进程列表与最近指标曲线。
- **连进去**：Web 终端（浏览器里直接 SSH）、串口（只读/接管，带录制）。
- **改文件**：设备文件管理（浏览、编辑、Hex、下载上传），仓库文件可分发到设备。
- **管服务**：systemd 单元启停、日志；也可以给进程发信号。
- **批量**：分发任务（灰度、失败率闸门、重试、校验）、OTA 升级。
- **审计**：所有写操作都进审计；「概览」有实时事件流（上下线、会话、审计、告警、任务）。

要“跑一条命令”时，用 [作业](#/operations) 而不是终端：先 `plan` 拿到将要执行的内容与 hash，
再 `apply`，最后 `verify`。risk ≥ 2 默认进待审批，**必须由另一个账号批准**（自己批自己会被拒）。

> 设备侧必须先配 `--allow-exec` 白名单，否则 exec 返回 501。
> 白名单只从设备本地配置加载，管理面不能远程扩权。

让故障自己处理：在 [自愈规则](#/playbooks) 里把“告警 → 命令”固化成规则。规则只生成作业、不动手；
自动执行要三把锁同时成立：规则开 `auto_apply`、命令风险 ≤ 全局上限（默认 1，只读）、总开关打开。

## 6. 接入大模型 {#ai}

### ① 选服务商、贴 Key

打开「AI 助手」点右上角**模型设置**：选一家服务商（DeepSeek、Kimi、OpenAI、通义、GLM、本地 Ollama），
模型名会自动填好，你只需要贴一个 **API Key**。用自建网关时在「高级」里填 `…/v1` 地址和模型名。

保存前可以点**测试连接**：它发一次最小请求，回来说明耗时、实际路由到的模型名，或上游的原始错误
（`/v1` 写错、模型名不对、令牌没权限都能看出来）。

**配置属于你自己的账号**：别人登录看到的是他自己那份，互不影响，也看不到你的密钥。
密钥只写到 relay 本机文件（`<admin.db 同目录>/ai/<账号>.key`，0600），不进数据库、不回显。

```bash
# 也可以用接口（令牌属于哪个账号，就配哪个账号）
curl -k -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"provider":"deepseek","model":"deepseek-chat"}' \
  https://relay:9443/api/v1/ai/config
curl -k -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"key":"sk-..."}' https://relay:9443/api/v1/ai/key
curl -k -X POST -H "Authorization: Bearer $ATK" https://relay:9443/api/v1/ai/test

# 自建网关：provider 选 custom，地址和模型都自己填
curl -k -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"provider":"custom","endpoint":"https://gw.example.com/v1","model":"deepseek-v4.1-flash"}' \
  https://relay:9443/api/v1/ai/config
```

### ② 用对话查现状

问“能不能部署到某台主机”这类问题时，它会先直接说清楚平台的边界（只能管已经接入 agent 的
设备，没有 agent 的主机要先手工接入），并给出安装/验证步骤，不会拿一堆查询把轮次耗光。
它有一份随程序发布的平台手册（部署、设备、会话、任务、证书、计费、权限、排障的口径与边界），
每次对话会自动挑相关章节，还会带上你的权限范围、平台总览和设备预览——所以不用先教它环境。
真查了很多轮还没结论时，它会自己收尾、先把已经查到的说清楚，不会让你把问题拆小再问。
没有查询权限的账号也能问“平台能做什么/该找谁”，它会回答边界而不是报“没有工具”。

**登录后的首页就是 AI 助手**，不用先想该点哪个页面；仪表盘在「概览」里。页面像常见聊天工具：
中间是问候和对话，下面是输入框，右上角是设备上下文、历史、新对话和模型设置。消息一条条往下追加，
头部和输入框跟着页面滚；往上翻看历史时右下角会出现「回到底端」，点一下回到输入框。问它什么都能答：

| 问什么 | 查的是 |
|---|---|
| 平台现在怎么样、有没有异常 | 总览计数、mTLS/ACL、告警状态 |
| 哪些设备离线 / 哪台负载高 | 设备、明细、分钟级历史指标 |
| 谁正连着设备 / 有多少会话 | 活跃会话、接入客户端 |
| 升级到哪了 / 为什么失败 | 分发与 OTA 任务 |
| 有几个客户 / 谁的额度快满了 | 租户、套餐、当月用量 |
| 这个月大概多少钱 | 用量 × 价目的费用试算 |
| 有哪些客户账号 / 谁被停用 | 接入账号与令牌 |
| 谁有管理台账号 / 哪个组给了什么权限 | 账号、权限组、成员 |
| 哪些证书快过期 / 设备会连哪台 relay | 证书台账、中继列表 |
| 仓库里有没有某个包 | 文件仓库 |

模型只能调用**只读工具**，工具还会按你自己的权限裁剪：没有证书权限就看不到证书工具，
问别人的设备会被拒，和你在页面上点不开是一样的。

「概览 → AI 诊断」会一次性把平台总览、离线设备、失败任务、快过期证书和告警汇总成排查建议。

### ③ 历史会话

每轮问答都会自动保存，**永久保留**：刷新、关页面、换设备重新登录都还在。

- 助手右上角「历史」列出最近 10 个会话，标题就是你的第一句话（可改名）
- 「加载更早的会话」继续往前翻，不会只留最近几条
- 点开历史会话就能接着问，上下文会带过去
- 会话按账号隔离：别人的对话你看不到，你的也别人也看不到
- 只有你点「删除」才会消失；删除不可恢复

### ④ 让它动手，但要你点头

直接说“把 nginx 重启一下”“清理 /tmp 下的旧包”，模型会先生成一张**作业确认卡**：
显示完整命令、风险级别、以及是否需要审批。它**不会自己执行**——低风险（只读命令）点“执行”即可；
改状态或破坏性命令要先“批准”，还要走配额与变更窗口。

也可以让它把这次处理写成**自愈规则草稿**，创建出来默认停用，你确认后再启用。

### ⑤ 给外部 agent / 脚本用

把接口暴露成 MCP 工具：默认只读，写操作要显式开启；调用带 `X-Actor` 标记进审计。

```bash
# OpenAPI（构建期从接口文档生成，与路由表对表）
curl -k -sS https://relay:9443/api/v1/openapi.json | head

# MCP server（stdio，只读；加 --allow-write 才暴露写接口）
python3 /opt/rtunnel/relay/deploy/rt-mcp.py --rt https://relay:9443 \
  --atk-file /etc/rtunnel/atk --insecure
```

> 模型看到的设备内容（进程名、文件、日志）都是不可信输入。不要给它持有终端做自动化，
> 也不要让它决定授权；写操作一律走「作业」的审批闸门。

## 7. 常见问题 {#faq}

| 现象 | 先查什么 |
|---|---|
| 设备显示离线 | 设备上 `ping` relay 的 18443；agent 日志是否在重连；relay 侧 `grep "agent online"`。多 relay 时按列表优先级重试，故障地址有 30s 冷却。 |
| 接口 401 | 会话过期或令牌失效；`atk-` 要放在 `Authorization: Bearer` 里。 |
| 接口 403 | 缺资源键，或设备不在授权范围。去「权限组」补键，或确认设备范围。 |
| exec 返回 501 | 设备 agent 没配 `--allow-exec`，或命令不在白名单里。 |
| 页面是旧的 | 换 relay 时没换前端。用内嵌版二进制，或同时更新 `admin/web`。 |
| AI 返回 503 | 提示会说明缺什么：没选服务商、没填模型名、没保存 Key；按提示补即可。 |
