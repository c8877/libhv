# 管理台

relay 加 `--admin 127.0.0.1:9443` 后由 libhv HttpService 提供（独立 worker 线程）。
页面在 `relay/admin/web`（Vue 3 + Element Plus），接口在 `relay/admin/backend`。
页面设计规范见 [`../uiue.md`](../uiue.md)。

## 页面清单

| 组 | 页面 | 做什么 |
|---|---|---|
| 工作台 | AI 助手（`/`） | 自然语言问平台、设备、告警；可提议作业与自愈规则 |
| 工作台 | 概览 | 待处理磁贴（告警/待审批/失败任务/证书到期）、KPI、事件流、AI 诊断 |
| 设备与链路 | 设备 | 在线设备列表、详情（概览/存储/文件/服务/进程/网络/监控/串口/能力） |
| 设备与链路 | 客户端 | `a:<user>` 列表，Shell 按钮按需拉起 agent |
| 设备与链路 | 会话 | 实时会话、流量、踢会话 |
| 设备与链路 | 中继列表 | 地址字典 + 下发列表 + 同步任务（见 [`README.md`](README.md)） |
| 文件与升级 | 文件仓库 | 仓库文件管理、部署到设备、分发入口 |
| 文件与升级 | 分发 | 选文件 → 选设备/时间窗/重试 → 每设备一条任务 |
| 文件与升级 | 任务 | 分发与 OTA 任务列表（按 `kind` 过滤） |
| 文件与升级 | Agent 升级 | OTA 包选择、按架构过滤设备、灰度/重试 |
| 接入与自动化 | 部署 | SSH 到内网主机装组件、扫网段（见 [`deploy.md`](deploy.md)） |
| 接入与自动化 | 作业 | plan→apply→verify，待审批作业的批准/拒绝/执行 |
| 接入与自动化 | 自愈 | 告警→命令规则、触发流水、立即执行 |
| 组织与权限 | 租户 / 账号 / 接入账号 / 权限组 / 授权对象 / 字典库 | 见下节 |
| 用量与费用 | 用量 / 费用 | 日聚合用量、价目、结算单 |
| 开放接口 | API Token / 接口文档 / 使用指南 | atk 管理、按角色列接口、操作手册 |
| 系统 | 告警 / 证书 / 审计 / 设置 | 告警确认、CA 签发与吊销、审计检索、全局配置 |

菜单显示名只由 `router.js` 的 `meta.title` 决定；`shared/nav.js` 只管结构、图标、权限键。

## 账号与权限

管理台账号存 SQLite（`--admin-db`），需要 OpenSSL 构建（PBKDF2 口令哈希）。

- 口令策略：默认最短 8 位，连续 5 次失败锁定 5 分钟（可在设置调整）；改密/禁用会吊销全部会话。
- 会话：HttpOnly Cookie，默认 12h。
- 接口：`GET/POST /api/v1/users`、`PUT /api/v1/users/{id}/password|status`、
  `DELETE /api/v1/users/{id}`、`POST /api/v1/login|logout`、`GET /api/v1/me`。

### 权限只来自权限组

每个动作对应一个资源键（`perm_resources`）。权限按资源、设备、端口三元求值：

- 功能资源：`perm_resources`（代码登记）+ `perm_group_resources`（组勾选）。
- 设备资源：`res_devices`（agent 上线自动登记）+ `perm_group_devices`（组授权）。
- 权限组另有 `all_devices`（含以后新增）、`global_scope`（全局权限）、`ports`（组级端口范围）。
- 组成员 `subject_type` 四种：`user`（管理台账号）、`atk`（管理台 Token）、
  `client`（接入账号）、`rtk`（接入 Token）。
- 账号/接入账号的有效权限 = 所属组的资源并集、设备并集。
- **令牌只能收窄**：atk/rtk 有效权限 = 归属主体 ∩ 令牌所属组；没有组时完全继承归属，
  永远不能超过归属主体。
- **`users.role` 不直接授权**：它只是内置组的别名。新建账号自动加入同名内置组，
  组成员变化时把 `role` 同步成最高的内置组（没有内置组归 viewer）。移出全部组就是零权限，
  不存在“按角色回落”。
- 设备不能直接绑到账号：只能通过权限组的设备授权获得范围。
- 超级权限来自「超级管理员」组（`super=1`），没有 `role=admin` 的硬编码；超级组至少保留
  一个账号成员，内置角色组不能删除。
- 预置 4 组：超级管理员、管理员、操作员、观察者；首次启动写入一次，之后不被重启覆盖。

| 角色 | 初始内置组 | 预置资源 |
|---|---|---|
| admin | 超级管理员（super） | 全部资源 + 全部设备 |
| manager | 管理员 | 除系统设置、关机、证书写、删账号外的全部资源（全局权限默认开） |
| operator | 操作员 | 设备查看/终端/端口转发/服务/串口读写、文件读写、分发读、仓库读写、权限读、账号读、设置读 |
| viewer | 观察者 | 设备查看、串口读、设置读、告警读 |

资源分「设备级」与「全局」两类：仓库 `repo.*`、中继 `relay.*`、字典/权限组/账号/令牌/
证书/审计/设置/告警属于全局资源，需要组开启「全局权限」（勾「全部设备」或 `super` 自动视为开启）。
这样受限设备的委派管理员也能管用户和仓库，但借全局键影响不到范围外的设备。

接口：

```
GET /api/v1/permission/resources            PUT /api/v1/permission/resources/{key}
GET /api/v1/permission/groups               GET /api/v1/permission/groups/{id}
POST /api/v1/permission/groups              PUT /api/v1/permission/groups/{id}
GET/PUT /api/v1/permission/groups/{id}/members
GET /api/v1/permission/subjects             GET/PUT /api/v1/permission/subjects/{type}/{id}/groups
GET /api/v1/permission/dicts?kind=resources|devices|ports|ttys
PUT /api/v1/permission/devices/{devid}      # 改名 / 临时停用
```

页面：「权限组」用成员/功能资源/设备资源三个 tab，成员与设备用穿梭框；「授权对象」按
账号/atk/接入账号/rtk 查看并分配组；「字典库」管四类字典。菜单与按钮按 `/api/v1/me`
返回的 `perms`/`super` 显示，**服务端始终二次校验**。

### atk 与 rtk

| | atk（管理台 API Token） | rtk（接入 Token） |
|---|---|---|
| 归属 | `users` 管理台账号 | `client_accounts` 接入账号 |
| 用途 | 管理台 HTTP `Authorization: Bearer atk-…` | relay 客户端；也可调管理台 HTTP |
| 权限公式 | 归属账号 ∩ 令牌组 | 接入账号 ∩ 令牌组 |
| 数据面 | 不参与 | 账号级规则 + 令牌级规则（配了组就只认令牌规则，空组 = 拒绝） |
| 管理入口 | 「API Token」页 | 「接入账号」详情页 |

一句话：**atk 是账号的管理台钥匙，rtk 是接入账号的设备钥匙**。

atk 细节：

- 格式 `atk-<64 位十六进制>`；库存 SHA-256 与前缀，明文只在创建/轮换时返回一次。
- 不加入任何组时完全继承归属账号；加入组后只能收窄。
- manager 只能管自己的 Token，admin 可管全部；只有 admin 能改分组绑定。
- Token 不能管理 Token（防凭据自我复制）；停用 Token/账号或到期后立即失效。
- 接口：`GET/POST /api/v1/admin-tokens`、`PUT /api/v1/admin-tokens/{id}`、
  `POST /api/v1/admin-tokens/{id}/rotate`、`DELETE /api/v1/admin-tokens/{id}`。

```bash
TOKEN=atk-...
curl -H "Authorization: Bearer $TOKEN" https://relay:9443/api/v1/agents

# 直接上传到文件仓库（operator 及以上）
curl -H "Authorization: Bearer $TOKEN" --data-binary @app.tar.gz \
     "https://relay:9443/api/v1/files/app.tar.gz"
```

## 设备详情页

- 单层 tab：概览 / 存储 / 文件 / 服务 / 进程 / 网络 / 监控 / 串口 / 能力。
  非活动 tab 不挂载、不请求，首次进入才加载，之后保留状态。
- 一级 tab 同步到 URL 的 `?tab=`，刷新后保持，前进/后退可切。
- 摘要条（设备名、agent 版本、连接信息、流量、采集时间）与 tab 一起吸顶。
- 概览：CPU/内存/负载/网络四个 KPI、CPU/内存趋势、存储用量、网卡、会话、系统信息与
  mTLS 证书状态。
- 表格默认 10 行/页，可选 5/10/20/50；进程、服务、网络、分发/OTA 任务在服务端排序后分页，
  只回当前页；网卡/串口/文件列表在浏览器端排序。数值列默认降序、文本列默认升序。
- 「能力」tab 展示运行时能力（端口/Shell/SYSINFO/mTLS/OTA/重启关机/文件/服务/串口白名单），
  `device.caps.write` 可在线编辑并落盘；设备本地页只读。
- 监控 tab 内切「曲线 / 采样配置」；指标下拉可「添加进程…」，进程曲线单独成图。
  指标 chip 的勾选按设备记在浏览器本地。
- 存储 tab：分区可折叠，展开只取一级子项，点目录按路径请求；右键「全展开」才拉整棵子树。
  目录按缩进 + 条形画成瀑布（条长按分区容量归一），<64KB 的文件按目录聚合成「小文件 N 项」，
  保证子项之和等于父项。眼睛按钮切换隐藏项；节点缓存 5 分钟。
- 页面实现是 `components/DeviceView.vue`，与设备本地页共用；数据来源不同（relay 代理 vs
  agent 直连），渲染与交互只有一份。

设备侧采集能力、接口参数、线程模型见 [`../agent/README.md`](../agent/README.md)。

## 文件仓库与分发

「文件仓库」是 relay 本机的 `--admin-file-dir`；设备文件在设备详情的「文件」tab。
两处挂**同一个** `rtunnel/shared/web/fs-ui.js`：设备侧走 WebSocket，仓库侧传一个 HTTP 后端
（`shared/repo-backend.js`）；仓库是扁平目录，所以隐藏路径栏与新建/重命名/移动。

- 表格与设备管理器一致：勾选列、名称/大小/修改时间（可排序）、搜索、底部汇总与批量操作。
- 行操作通过 `⋯`、右键或触屏长按 500ms：部署到设备、分发到多台、下载、编辑、Hex、尾部、
  复制 SHA-256、属性、删除。
- 批量：下载（逐个）、打包下载（浏览器内生成 tar，可 gzip）、删除；删除在页面内展开确认条，
  不弹窗。
- 「部署」是行内表单：目标设备只列在线设备，默认沿用上次成功的设备；目标路径默认上次目录 +
  文件名，必须绝对路径；`push_open` 由 relay 单独开一条 `OPEN_FS` 流，浏览器只收进度，
  不中转文件内容；同名默认不覆盖并标冲突。
- 仓库文本编辑走 `PUT /api/v1/files/<name>`（manager+，原子写，默认上限
  `file.max_edit_mb = 4 MiB`）。
- 分发：文件页「分发」→ 选设备/时间窗/重试 → 每设备一条任务。
  `POST /api/v1/distributions` 创建，`GET /api/v1/distributions` 查询；任务页支持状态过滤、
  重试、取消、超时延长。同设备同哈希重复分发标「冲突」，可校验设备文件（相同则直接成功）
  或强制重发。
- 部署包也放在这里：`packages/<组件>/<版本>/<架构>/xxx.tar.gz`，OTA 与「部署」共用同一份。

## 用量与费用

用量本身由 relay 数据面按 5s 增量采集（见 [`README.md`](README.md#运维)），页面只做展示与计费：

- **用量**：按日或总计查 `GET /api/v1/usage`（`usage.read`）；页面上有合计/日均/峰值与按天柱状图，
  明细按「天 × 主体」列出，原始流水保留 180 天（`usage.raw_retention_days`），日聚合长期保留。
- **费用试算**：`price_rules` 存每个指标的计价单位与单价（金额以 1e-6 元存整数），
  `GET /api/v1/billing/summary` 把日聚合用量乘单价、按租户汇总；「用量与费用 → 费用」可改价目
  （`billing.write`，默认只有超管）。
- **结算单**：按账期把金额与明细冻结成 `billing_statements`（`draft → issued → paid`，可作废），
  出账后不随用量变化；可导出 CSV 交给财务。仍未接支付与发票。
- **租户**：设备与接入账号各属一个租户（默认 id=1），数据面按租户强制隔离；停用租户等于停用
  其下全部接入账号并断开在线连接。租户可设套餐：设备上限（分配时校验）、并发上限、月流量上限
  （超流量断开该租户全部令牌并拒绝新会话），0 表示不限。

## 访问设备 Web

### Web 终端（免密）

admin 作为 RTNP client 直连本机 relay，向目标 agent 发 `OPEN_SHELL`（flags=0x0001），
agent `forkpty` 起交互 shell，经 WebSocket 接到浏览器 xterm.js。不 fork ssh、不经手密码。

- 身份固定为管理台内部代理（relay 启动时生成的进程内密钥，不落盘、不配置），
  **不接受浏览器指定用户**；操作者权限与设备范围在管理台侧校验。
- 设备列表 devid = 设备 ID；客户端列表 devid = `a:<user>`（按需拉起）。
- 需要目标 agent 开 `--allow-shell`；终端尺寸经 `RESIZE`(0x17) 帧同步。
- 前端支持多 tab；切换页面只摘 DOM，不关闭 WebSocket 会话。

### 设备 Web 代理（路径式）

板子上的 Web 服务可通过管理台直接访问，不需要 A 端或本地代理：

```
http://B:9443/device/<devid>/<port>/<path>
例：http://192.168.1.133:9443/device/board01/8080/
```

- 认证：管理台 Cookie 会话；普通账号只能访问绑定到自己账号的设备。
- 端口必须在 agent 的 `--allow` 列表里；数据面是 admin 作为 RTNP client 连本机 relay。
- HTTP 响应流式转发（chunked），`Location` 与 HTML/CSS 绝对路径做前缀改写；同时支持 WS。
- 设备 Cookie 加 `d_<devid>_<port>_` 前缀，与管理台会话隔离。
- 限制：同时最多 32 条代理请求（超出 503）；不支持设备侧 HTTPS；relay 需明文部署。

## AI 助手

登录后的首页就是对话页（参考 DeepSeek chat：居中对话、底部输入框、历史会话抽屉），
仪表盘在「概览」。设计细节见 [`../uiue.md`](../uiue.md) 的「对话页」。

能力：

- 问平台总览、告警、设备与会话、升级任务、租户与费用、客户账号、权限组、证书、中继、
  文件仓库；也能问“谁改过权限”“哪些证书快过期”“这个月多少钱”。
- 说“重启一下 nginx”“清理 /tmp 旧包”会生成**作业确认卡**（完整命令 + 风险级别），
  **不会自己执行**：只读命令点「执行」，改状态/破坏性命令要先「批准」，照样走配额与变更窗口。
- 能把排障结论写成**自愈规则草稿**，创建出来默认停用。
- 19 个只读工具 + 2 个提议工具，**按账号权限裁剪**（没证书权限就看不到证书工具），
  工具内部再按资源权限与设备范围收窄。
- 没有 `device.view` 的账号也能用：只靠手册与实时上下文回答边界与步骤，不会 403。
- 工具轮次 24 轮 + 180s 预算；同名同参重复调用会被截断；无论怎么停下都会再要一次不带工具的
  收尾回答，收尾失败也会说明“查过什么”。

平台知识在 `relay/admin/backend/ai/prompt_library.md`（构建期嵌入）。**新增/修改平台能力必须
同步这份手册**，内容指纹会自动让预热备忘失效。回答规则不要写回 `api_ai.cpp` 的长字符串。

模型配置按账号隔离：在「AI 助手 → 更多 → 模型设置」选服务商（DeepSeek / Kimi / OpenAI /
通义 / GLM / 本地 Ollama / 自定义网关）、确认模型名、贴 API Key、点「测试连接」。
API Key 只写 relay 本机 `<admin.db 同目录>/ai/<账号>.key`（0600），不进数据库、不回显。

历史会话每轮自动落库、**永久保留**（只有手动删除才消失），按账号隔离；「历史」列出最近
10 个，可往前翻、改名/删除，点开是完整对话（工具调用与确认卡都还原）。

## 开放接口（OpenAPI / MCP）

```bash
# OpenAPI：不需要令牌，和首页同级
curl -k -sS https://relay:9443/api/v1/openapi.json | jq '.info, (.paths|length)'

# 工具清单（默认只读）
python3 rtunnel/deploy/rt-mcp.py --rt https://relay:9443 \
  --atk-file /etc/rtunnel/atk --insecure --list-tools

# 作为 MCP server 挂在客户端下
python3 rtunnel/deploy/rt-mcp.py --rt https://relay:9443 \
  --atk-file /etc/rtunnel/atk --insecure
```

```json
{"mcpServers": {"rtunnel": {"command": "python3", "args": [
  "/opt/rtunnel/relay/deploy/rt-mcp.py",
  "--rt", "https://relay:9443",
  "--atk-file", "/etc/rtunnel/atk", "--insecure"]}}}
```

约定：

- **schema 单源**：`api-docs.js` →（构建期）`dist/openapi.json` →（运行期）MCP 工具，
  不手写第二份。`npm run check:docs` 会与 `admin_routes.cpp` 对表。
- **默认只读**：只有 GET 变成工具；`--allow-write` 才暴露写接口，`--allow-prefix` 可再收窄。
- **不绕过授权**：工具只是 API 包装，能不能调仍由令牌资源键与设备范围决定；写操作建议走
  `operations` 的 plan→apply→verify。
- **审计可区分**：MCP 请求自动带 `X-Actor: ai:rt-mcp`（`RT_ACTOR` 可覆盖），审计里显示成
  `ai:rt-mcp|账号#id/…`。
- 令牌只从文件/环境读：`--atk-file`（0600）或 `ATK`，不接受命令行明文。

## 告警、审计、证书

### 告警

设置页「告警」组配总开关 + webhook，支持 `generic` / 企业微信 / 钉钉（加签）/ 飞书；
事件为设备离线/恢复、证书到期（预警与紧急两级）、分发任务失败/超时；同类事件按冷却时间去重。
`GET /api/v1/alerts/status` 看发送结果，`POST /api/v1/alerts/test` 验证通道。
webhook URL 与签名密钥对非 admin 脱敏，审计不记明文密钥。

告警事件同时落 `alert_events`（保留最近 2000 条），在「系统 → 告警」里确认：

- `GET /api/v1/alerts?state=open|acked|all&limit=`（默认 100，上限 500），返回 `counts`。
- `POST /api/v1/alerts/:id/ack`、`POST /api/v1/alerts/ack-all`（`alert.write`）。
- 确认只是标记“人看过了”，不改变设备状态；概览的「待处理」磁贴统计的是未确认数。

### 审计

- 存储：`audit.log` + `audit.log.jsonl`，字段 `at/actor_type/actor/action/target/result/detail/op_id`；
  `actor_type` 含 user/client/token/ai。
- 页面按 `ok/denied/error` 分色，可按结果筛选、按时间/操作者/操作/对象/结果排序、导出 CSV，
  时间按本地时区显示；内存保留最近 1000 条（`GET /api/v1/audit?limit=` 最大 1000）。
- 越权请求（403）写一条 `perm.denied`，detail 带 `path/need/role`——能查到“谁试图做什么被拒”。
- 落盘的 `.jsonl` 目前没有查询接口，按时间范围查历史需要另外实现。

### 证书

`--admin-ca-cert/--admin-ca-key/--admin-cert-dir` 配 CA 与台账；台账 `index.txt`、黑名单
`revoked.txt`、审计 `audit.log` 都在目录下，启动自动加载黑名单。管理台可签发/吊销设备证书，
证书到期前由告警模块提醒。

## 前端构建

```bash
cd relay/admin/web
npm run build          # → dist/，配合 --admin-docroot
npm run check:ui       # 组件规约检查
npm run check:docs     # 接口文档对表
```

不带目录时用内嵌版：`cmake -DRTUNNEL_ADMIN_WEB=ON` 后构建
`rtunnel-relay-embedded`，二进制自带前端（构建期把 `web/dist` 预压缩体嵌入，约 +0.8 MB）。
指定 `--admin-docroot` 时仍以磁盘为准，磁盘缺文件再回退内嵌。
