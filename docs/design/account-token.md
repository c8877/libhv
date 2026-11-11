# 接入账号与 Token（设计稿）

状态：P1–P3 已实现；P4 仅完成审计与文档，旧凭据导入未做（原因见 §10）  
关联：`docs/permission.md`、`docs/admin/permission.md`、`docs/design/rtunnel.md`

---

# 1. 一句话

账号是常驻的权限主体，Token 是账号签发的一次性可见、可独立限速/限额/过期的临时身份牌；客户端拿 Token 接入，relay 解析出账号，按账号现有分组 ACL 授权，再叠加 Token 自己的时效、流量、频率限制。

# 2. 背景与现状

当前 A 端认证只有两种方式：

```
--client-users F     每行 user:token，启动时读入内存
--client-token T     全局 token
```

问题：

```
账号与凭据绑死      换 token 要改文件、重启或 SIGHUP
无法按凭据限额      所有 token 共享同一套能力
无法单独吊销        只能删用户或改文件
无法审计到具体凭据  日志只有 user，没有 token 身份
```

权限侧已经按用户名建模（`client_group_members.username`），因此账号可以直接复用现有 ACL 求值路径，不需要改权限模型。

# 3. 身份模型

```
账号 Account        常驻，权限主体，用户名唯一且不可改
  │
  ├── Token A       临时身份牌，可过期/停用/轮换
  ├── Token B       同一账号，不同限额，互不影响
  └── Token C
```

三条硬规则：

```
1. Token 不能扩权        权限永远等于账号的 client group ACL
2. Token 只能收窄        通过时效、流量、频率、并发做限制
3. 账号停用即全停        该账号所有 Token 同时失效，在线连接被踢
```

命名：

```
客户端接入账号    client account    本设计新增，管理台「接入账号」
管理台登录账号    admin user        现有 users 表，不变
```

# 4. 需求

## 4.1 账号

| 项 | 要求 |
|---|---|
| 创建 | 管理台创建，用户名唯一，≤64，创建后不可改名 |
| 状态 | `active` / `disabled`；停用立即拒绝新认证并踢掉在线连接 |
| 展示名 | `display_name` 可改，仅用于界面 |
| 备注 | 可选，≤256 |
| 删除 | 级联删除 Token、客户端分组成员；需二次确认 |
| 权限 | 复用现有客户端分组，不新增权限模型 |

## 4.2 Token

| 项 | 要求 |
|---|---|
| 格式 | `rtk_` + 32 字节随机数的 base64url，共 47 字符 |
| 存储 | 只存 SHA-256 十六进制；明文仅在创建/轮换响应中出现一次 |
| 数量 | 单账号默认最多 20 个（可配置） |
| 名称 | 用户可读标签，≤64，便于区分用途 |
| 状态 | `active` / `disabled` / `revoked` |
| 有效期 | 永久或绝对时间；到期立即失效 |
| 流量配额 | 总流量 / 每日 / 每月；0 = 不限 |
| 频率限制 | 每分钟控制请求数 + 突发；0 = 不限 |
| 并发限制 | 同时存在的隧道会话数；0 = 不限 |
| 轮换 | 生成新 Token 并立即吊销旧 Token |
| 删除 | 逻辑吊销，保留审计与用量记录 |

## 4.3 认证

```
1. 客户端 HELLO
2. AUTH 携带 auth_type=2 + token，用户名可空
3. relay 对 token 做 SHA-256，查运行时缓存
4. 校验 token 状态、有效期、账号状态、周期配额
5. 通过后 c->user = 账号用户名，后续 ACL 完全复用现有逻辑
```

兼容要求：

```
旧客户端（auth_type=0/1）行为不变
旧 --client-users / --client-token 继续可用
未启用管理台时，退化为纯静态凭据，Token 功能不可用
```

## 4.4 限额语义

### 时效

```
AUTH 时校验          过期直接 AUTH_FAIL(TOKEN_EXPIRED)
在线期间到期          每 5s 扫描，断开该 Token 的所有连接
```

### 流量

```
计量对象    DATA 帧载荷，双向相加（A→C 与 C→A）
计量点      relay 转发每条 DATA 时累加
到达配额    cut   立即 RESET 所有会话并断开（默认，硬上限）
            block 只拒绝新 OPEN，已有会话可继续
周期重置    daily 按 UTC 零点；monthly 按 UTC 每月 1 日零点
持久化      每 5s 由管理台线程拉取快照落库，进程退出前再刷一次
```

### 频率

```
计量对象    OPEN + DEVICE_QUERY（控制面请求），PING/DATA 不计
算法        令牌桶：rate_per_min / 60 每秒补充，burst 为桶容量
OPEN 超限   OPEN_FAIL(RATE_LIMITED)，消息带 retry_after=毫秒
查询超限    DEVICE_STATUS 返回 online=0（客户端按离线处理，不断链）
```

### 并发

```
计量对象    该 Token 当前活跃 RelayStream 数
超限返回    OPEN_FAIL(SESSION_LIMIT)
```

## 4.5 管理台

```
接入账号页
  账号列表：用户名 / 状态 / Token 数 / 最近使用 / 创建时间
  行展开：Token 列表 + 用量
  操作：新建账号、停用、删除、有效权限、导入旧凭据

Token 列表
  名称 / 前缀 / 状态 / 到期 / 流量进度 / 频率 / 并发 / 最近使用
  操作：新建、停用/启用、轮换、删除

新建 Token
  名称、有效期、流量配额与周期、频率与突发、并发、超限动作
  创建成功后一次性展示完整 Token，离开弹窗不可再查看
```

权限要求：

```
viewer    只读，看不到任何完整 Token
operator  查看用量
manager   创建/停用/轮换 Token
admin     删除账号、删除 Token、导入旧凭据
```

## 4.6 审计与安全

```
审计事件
  client_account.create / update / disable / enable / delete
  client_token.create / update / disable / enable / rotate / delete
  client_token.auth_fail（前缀 + 原因 + IP，按分钟聚合）
  client_token.quota_exceeded

禁止记录
  完整 Token、Token 哈希
日志只允许 token_prefix 与 token_id
```

# 5. 详细设计

## 5.1 总体结构

```
                    ┌────────────────────────────────┐
                    │ 管理台 admin (9443)            │
                    │ client_accounts / client_tokens│
                    │ 创建、停用、配额、用量展示      │
                    └───────────────┬────────────────┘
                                    │ callRelay(setTokenCache)
                                    │ 每 5s 拉取用量快照
                                    ▼
┌──────────┐  AUTH(type=2,token)  ┌──────────────────────────┐  OPEN/DATA  ┌──────────┐
│ client A │ ───────────────────► │ relay                    │ ──────────► │ agent/C  │
│          │ ◄─────────────────── │ token cache: hash→policy │ ◄────────── │          │
└──────────┘      AUTH_OK         │ usage: id→计数/令牌桶     │             └──────────┘
                                  └──────────────┬───────────┘
                                                 │ 用量快照
                                                 ▼
                                          admin.db (SQLite)
```

关键约束：

```
relay 事件循环不访问 SQLite
管理台把策略推给 relay，relay 把用量推回管理台
所有限流、计量都在 relay 单线程内完成，无锁
```

## 5.2 数据模型

```sql
CREATE TABLE IF NOT EXISTS client_accounts(
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  username      TEXT NOT NULL UNIQUE,
  display_name  TEXT NOT NULL DEFAULT '',
  status        TEXT NOT NULL DEFAULT 'active',   -- active | disabled
  note          TEXT NOT NULL DEFAULT '',
  created_by    TEXT NOT NULL DEFAULT '',
  created_ms    INTEGER NOT NULL,
  updated_ms    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS client_tokens(
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  account_id      INTEGER NOT NULL,
  name            TEXT NOT NULL DEFAULT '',
  token_hash      TEXT NOT NULL UNIQUE,           -- sha256(明文)，64 hex
  token_prefix    TEXT NOT NULL,                  -- rtk_9f2c8a1b，仅展示
  status          TEXT NOT NULL DEFAULT 'active', -- active|disabled|revoked
  expires_at_ms   INTEGER NOT NULL DEFAULT 0,     -- 0 = 永久
  quota_bytes     INTEGER NOT NULL DEFAULT 0,     -- 0 = 不限
  quota_period    TEXT NOT NULL DEFAULT 'total',  -- total|daily|monthly
  period_start_ms INTEGER NOT NULL DEFAULT 0,
  used_bytes      INTEGER NOT NULL DEFAULT 0,
  rate_per_min    INTEGER NOT NULL DEFAULT 0,     -- 0 = 不限
  burst           INTEGER NOT NULL DEFAULT 0,     -- 0 = 取 rate_per_min
  max_sessions    INTEGER NOT NULL DEFAULT 0,     -- 0 = 不限
  quota_action    TEXT NOT NULL DEFAULT 'cut',    -- cut|block
  created_by      TEXT NOT NULL DEFAULT '',
  created_ms      INTEGER NOT NULL,
  last_used_ms    INTEGER NOT NULL DEFAULT 0,
  last_used_ip    TEXT NOT NULL DEFAULT '',
  revoked_ms      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_client_token_account ON client_tokens(account_id);
```

ER 草图：

```
client_accounts 1 ────────── n client_tokens
  id                            id
  username (unique)             account_id
  status                        token_hash (unique)
  display_name                  token_prefix
  created_ms                    status
                                expires_at_ms
                                quota_bytes / quota_period
                                period_start_ms / used_bytes
                                rate_per_min / burst
                                max_sessions / quota_action
                                last_used_ms / last_used_ip
```

## 5.3 Token 生成与存储

```
随机源   CSPRNG 32 字节（RAND_bytes，无 OpenSSL 时用 getrandom/urandom）
编码     base64url 无填充，43 字符
明文     rtk_<43>
展示前缀 rtk_ + 明文前 8 字符 + "…"
哈希     SHA-256(明文 UTF-8) → 64 hex，唯一索引
比对     relay 计算哈希后 O(1) 查表；不做明文比较
```

落盘与回显：

```
创建/轮换响应   { "token": "rtk_...", "id": 12, ... }   仅此一次
列表响应        只有 token_prefix，没有明文和哈希
数据库          只有 token_hash 与 token_prefix
```

`SHA-256` 使用 `rtunnel/common/sha256.*` 自带实现，避免 relay 在无 OpenSSL 构建下无法校验 Token。

## 5.4 协议改动

AUTH 载荷：

```
str  identity          // auth_type=2 时可为空，由 token 解析
u8   auth_type         // 0=none 1=静态 token 2=账号 token
str  token
```

处理规则：

```
auth_type=2
  identity 为空      按 token 解析账号
  identity 非空      必须与 token 所属账号一致，否则拒绝
  账号不存在/停用    AUTH_FAIL(TOKEN_DISABLED)
  token 停用/吊销    AUTH_FAIL(TOKEN_DISABLED)
  token 过期         AUTH_FAIL(TOKEN_EXPIRED)
  周期配额已用完     AUTH_FAIL(QUOTA_EXCEEDED)
```

AUTH_OK 追加可选尾部（旧客户端不解析，直接忽略）：

```
str  username
u32  heartbeat_ms
u8   flags             // bit0=1 表示尾部存在
u64  expires_at_ms     // 0=永久
u64  quota_bytes       // 0=不限
u64  used_bytes
u32  rate_per_min
u32  max_sessions
```

新增错误码（`rtnp/frame.hpp`）：

```
TOKEN_EXPIRED    = 12
TOKEN_DISABLED   = 13
QUOTA_EXCEEDED   = 14
RATE_LIMITED     = 15
SESSION_LIMIT    = 16
```

客户端行为：

```
TOKEN_EXPIRED / TOKEN_DISABLED   打印原因，停止重连
QUOTA_EXCEEDED                   打印用量，停止重连
RATE_LIMITED                     OPEN 失败后按 retry_after 决定是否重试
SESSION_LIMIT                    关闭最旧会话或提示用户
```

## 5.5 认证与授权时序

```
client A                 relay B                        admin
   │                        │                             │
   │ HELLO                  │                             │
   │───────────────────────►│                             │
   │ AUTH user="" type=2    │                             │
   │ token=rtk_xxx          │                             │
   │───────────────────────►│                             │
   │                        │ sha256(token)               │
   │                        │ cache[hash] → policy        │
   │                        │ check status/expiry/quota   │
   │                        │ check account.status        │
   │                        │ mTLS: SAN == account        │
   │ AUTH_OK user=alice     │                             │
   │◄───────────────────────│                             │
   │                        │                             │
   │ OPEN dev1:22           │                             │
   │───────────────────────►│ 1 rate bucket               │
   │                        │ 2 ACL(账号 alice)           │
   │                        │ 3 session count             │
   │                        │ 4 quota remaining           │
   │                        │ 5 转发给 agent              │
   │ OPEN_OK                │                             │
   │◄───────────────────────│                             │
   │                        │                             │
   │ DATA ⇄                 │ used_bytes += len(DATA)     │
   │                        │ 每 5s ──用量快照───────────►│
   │                        │                             │ UPDATE used_bytes
```

## 5.6 运行时结构

```cpp
struct TokenPolicy {
    long long id = 0;
    long long account_id = 0;
    std::string username;
    std::string prefix;
    std::string name;
    int         status = 0;          // active|disabled|revoked
    int64_t     expires_at_ms = 0;   // 0=never
    int64_t     quota_bytes = 0;     // 0=unlimited
    int         quota_period = 0;    // 0 total, 1 daily, 2 monthly
    int64_t     period_start_ms = 0;
    int64_t     used_base = 0;       // 上次落库值
    int         rate_per_min = 0;
    int         burst = 0;
    int         max_sessions = 0;
    int         quota_action = 0;    // 0 cut, 1 block
    int         account_active = 1;
};

struct TokenUsage {
    int64_t  used_bytes = 0;         // 本期累计（绝对值）
    int64_t  period_start_ms = 0;
    double   bucket = 0;             // 令牌桶余额
    uint64_t refill_ms = 0;
    int      active_sessions = 0;
    uint64_t last_used_ms = 0;
    char     last_ip[64] = {0};
};

// relay 内，全部在事件循环线程访问
std::unordered_map<std::string, TokenPolicy> token_cache;  // hash → policy
std::unordered_map<long long, TokenUsage>    token_usage;  // token id → usage
```

`RelayConn` 增加：

```cpp
long long token_id = 0;
char      token_prefix[16] = {0};
```

`RelayStream` 不需要新字段，转发时取 `s->a->token_id`。

## 5.7 限流与配额

### 令牌桶

```
rate = rate_per_min / 60.0 次/秒
burst = burst > 0 ? burst : rate_per_min

每次 OPEN / DEVICE_QUERY：
  now = hloop_now_ms()
  补充 bucket = min(burst, bucket + (now - refill_ms) * rate / 1000)
  refill_ms = now
  若 bucket < 1：拒绝 RATE_LIMITED，retry_after = (1-bucket)/rate*1000
  否则 bucket -= 1

rate_per_min=0 表示不限，直接放行
```

示意：

```
tokens
 10 ┤••••••••••            burst=10
  8 ┤        ••••
  5 ┤            ••
  2 ┤              •
  0 └──┬────┬────┬────┬────► t
      OPEN OPEN OPEN OPEN
       -1   -1   -1   -1    每秒补 1 个
```

### 流量

```
每条 DATA：usage.used_bytes += f->len

转发后若 quota_bytes > 0 且 used_bytes >= quota_bytes：
  quota_action=cut
    sendErr(QUOTA_EXCEEDED) 所有该 token 会话
    断开连接，标记 exhausted
  quota_action=block
    标记 exhausted，不再接受新 OPEN，已有会话继续
```

### 周期重置

```
daily    下一个 UTC 零点
monthly  下一个 UTC 月首零点
total    不自动重置，只有管理员手动清零

扫描间隔 5s，由 relay 定时器执行：
  若 now >= period_start + 周期长度
    used_bytes = 0
    period_start_ms = 新周期起点
    清除 exhausted 标记
```

### 状态机

```
         创建
          │
          ▼
       ┌──────┐  停用   ┌──────────┐
       │active│◄───────►│ disabled │
       └──┬───┘  启用   └────┬─────┘
          │                  │
   到期   │                  │ 删除/轮换
          ▼                  ▼
      ┌───────┐         ┌─────────┐
      │expired│         │ revoked │
      └───────┘         └─────────┘
          │
   配额用完│（周期重置可回 active）
          ▼
      ┌──────────┐
      │ exhausted│
      └──────────┘
```

## 5.8 策略同步与用量落库

### admin → relay

```cpp
// AdminServer::refreshTokenCache()
1. 从 client_accounts + client_tokens 读全量
2. 组装 vector<TokenPolicy>
3. callRelay([=]{ relay->setTokenCache(policies); })
```

`setTokenCache` 在 relay 线程内：

```
替换 token_cache
保留 token_usage 中仍存在的 token id
若 DB period_start 比本地新，重置本地 used_bytes
若某 token 变为 disabled/revoked/过期，或账号停用：
  踢掉使用该 token 的在线连接
```

### relay → admin

```cpp
// relay 侧快照
struct TokenUsageSnapshot {
    long long id;
    long long used_bytes;
    long long period_start_ms;
    long long last_used_ms;
    std::string last_ip;
};

std::vector<TokenUsageSnapshot> RelayServer::tokenUsageSnapshot();
```

管理台起一个 5 秒循环：

```
snapshot = callRelay(relay->tokenUsageSnapshot())
UPDATE client_tokens
   SET used_bytes=?, last_used_ms=?, last_used_ip=?
 WHERE id=? AND period_start_ms=?
```

`period_start_ms` 作为乐观锁，避免周期重置后旧快照把用量写回去。

进程退出：

```
relay 在 hloop_run 返回后、admin stop 前刷一次
```

## 5.9 管理 API

| 方法 | 路径 | 说明 | 角色 |
|---|---|---|---|
| GET | `/api/v1/client-accounts` | 列表，支持 `q` 搜索 | operator |
| GET | `/api/v1/client-accounts/:id` | 详情，含 Token 统计与分组成员数 | operator |
| POST | `/api/v1/client-accounts` | 新建 `{username,display_name,note}` | manager |
| PUT | `/api/v1/client-accounts/:id` | 改展示名/备注/状态 | manager |
| DELETE | `/api/v1/client-accounts/:id` | 级联删除 | admin |
| GET | `/api/v1/client-accounts/:id/tokens` | Token 列表（不含明文） | operator |
| POST | `/api/v1/client-accounts/:id/tokens` | 新建 Token，返回一次明文 | manager |
| PUT | `/api/v1/client-tokens/:id` | 改名称/状态/限额 | manager |
| POST | `/api/v1/client-tokens/:id/rotate` | 轮换，返回一次明文 | manager |
| DELETE | `/api/v1/client-tokens/:id` | 吊销 | manager |
| GET | `/api/v1/client-accounts/:id/effective` | 有效权限，复用预览逻辑 | operator |

旧凭据导入接口未实现，见 §10.5。

新建请求示例：

```json
{
  "name": "构建机",
  "expires_in_days": 30,
  "quota_gb": 50,
  "quota_period": "monthly",
  "rate_per_min": 60,
  "burst": 10,
  "max_sessions": 8,
  "quota_action": "cut"
}
```

响应示例：

```json
{
  "id": 12,
  "name": "构建机",
  "token": "rtk_5Jd9...",
  "token_prefix": "rtk_5Jd9xQ2m...",
  "expires_at_ms": 1790000000000,
  "quota_bytes": 53687091200,
  "quota_period": "monthly",
  "rate_per_min": 60,
  "burst": 10,
  "max_sessions": 8
}
```

## 5.10 页面草图

侧边栏新增一项，放在「客户端 A」之后：

```
概览
设备
权限管理
客户端 A
接入账号      ← 新增
会话
证书
账号
文件
任务
审计
设置
```

账号列表：

```
┌ 接入账号 ──────────────────────────────────── [+ 新建账号] ┐
│                                                             │
│ 用户名      状态    Token   最近使用        操作            │
│ ▾ alice     启用     2      2 分钟前   [有效权限] [删除]    │
│   ┌───────────────────────────────────────────────────────┐ │
│   │ 名称      前缀         状态   到期      流量          │ │
│   │ 构建机    rtk_5Jd9xQ2m… 启用  30 天    12 / 50 GB      │ │
│   │ 巡检      rtk_71ab…   停用   已过期   0 / 不限        │ │
│   │                                     [+ 新建 Token]    │ │
│   └───────────────────────────────────────────────────────┘ │
│ ▸ bob       停用     1      3 天前     [有效权限] [删除]    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

新建 Token：

```
┌ 新建 Token ──────────────────────────────────┐
│                                               │
│ 名称        [ 构建机                        ] │
│                                               │
│ 有效期      ( ) 永久   (•) 30 天   ( ) 自定义 │
│                                               │
│ 流量配额    [ 50 ] GB    周期 [ 每月      ▾ ] │
│             0 表示不限                         │
│                                               │
│ 请求频率    [ 60 ] 次/分   突发 [ 10 ]        │
│ 并发会话    [ 8 ] 个       0 表示不限          │
│                                               │
│ 超限动作    (•) 断开连接   ( ) 只拒绝新连接    │
│                                               │
│                        [ 取消 ]  [ 创建 ]      │
└───────────────────────────────────────────────┘
```

创建成功（只显示一次）：

```
┌ Token 已创建 ────────────────────────────────┐
│                                               │
│ 只显示这一次，关闭后无法再次查看。             │
│                                               │
│ ┌───────────────────────────────────────────┐ │
│ │ rtk_5Jd9xQ2m...                           │ │
│ └───────────────────────────────────────────┘ │
│                                               │
│ [ 复制 ]                                      │
│                                               │
│ 丢失只能轮换，旧 Token 会立即失效。            │
│                                               │
│                                  [ 我已保存 ] │
└───────────────────────────────────────────────┘
```

## 5.11 权限页联动

```
客户端分组成员选择   改为从 client_accounts + 静态文件用户取候选
不存在提示           用户 alice 不是接入账号
有效权限预览         直接复用 permEffectiveClient(username)
```

Token 不参与分组，分组始终按账号用户名求值。

# 6. 兼容与迁移

## 6.1 运行模式

| 场景 | 行为 |
|---|---|
| 启用管理台 | DB Token + 静态凭据都接受 |
| 未启用管理台 | 仅静态凭据，Token 页面不存在 |
| 两者同名账号 | 账号状态以 DB 为准；静态 token 仍可认证 |
| 账号被停用 | 静态 token 同样被拒绝 |

## 6.2 导入旧凭据

```
入口    接入账号页 →「导入旧凭据」
输入    user:token 每行一条，或读取 --client-users 文件内容
结果    每个 user 建账号（已存在则复用），每个 token 建一条记录
        名称标记 legacy，状态 active，永久有效，限额不限
提示    导入后建议为旧 Token 设置有效期并逐步轮换
```

导入不删除 `--client-users`，运维确认全部切换后再移除。

## 6.3 配置项

| 配置 | 默认 | 说明 |
|---|---|---|
| `token.max_per_account` | 20 | 单账号 Token 上限 |
| `token.usage_flush_ms` | 5000 | 用量落库间隔 |
| `token.sweep_ms` | 5000 | 过期/配额扫描间隔 |

放进现有 `kDefs` 设置表，管理台可调。

# 7. 边界与风险

```
多 relay 实例     不支持；缓存和用量都在进程内，SQLite 单写
时钟             有效期以 relay 本机时间为准；部署需 NTP
计量精度         崩溃最多丢一个 flush 周期（5s）的流量
Token 泄露       仅存哈希；支持单独吊销与轮换
暴力猜测         256 位随机，不需要额外限速；AUTH 失败建议按 IP 限流
日志泄露         只记录 token_prefix，不记录明文和哈希
mTLS             Token 认证仍受 SAN 校验约束；SAN 必须是账号用户名
按需 agent       a:<user> 在同账号多连接时可能选错连接，沿用现有行为
```

# 8. 分期

```
P1  数据模型 + 账号/Token CRUD + SHA-256 + AUTH type=2 + 缓存推送
P2  令牌桶、并发限制、流量计量、周期重置、用量落库
P3  管理台页面、一次性明文、用量进度、有效权限入口
P4  旧凭据导入、审计完善、文档与运维说明
```

# 9. 验收用例

```
认证
  正确 Token 接入，解析出账号，ACL 与账号一致
  用户名与 Token 不匹配被拒
  停用 Token / 停用账号 / 过期 Token 被拒
  删除 Token 后旧连接被踢

权限
  Token 不能访问账号 ACL 之外的设备
  账号加入新分组后，Token 立即获得新权限

限额
  超过 rate_per_min 返回 RATE_LIMITED，retry_after 合理
  并发达到 max_sessions 返回 SESSION_LIMIT
  流量到配额后按 cut/block 行为执行
  daily/monthly 周期到点自动重置
  停用后重新启用，用量保留

兼容
  旧 --client-users 客户端不受影响
  未启用管理台时行为与现在一致
  旧客户端忽略 AUTH_OK 尾部字段

协议版本
  rtnp::VERSION 不递增，auth_type=2 与 AUTH_OK 尾部均为向后兼容扩展
  新客户端连接旧 relay 时会因 auth_type=2 被拒，回退到静态 token 即可
```

# 10. 实现说明

## 10.1 代码位置

```
rtunnel/common/sha256.*            自带 SHA-256（relay 无 OpenSSL 也能校验）
rtunnel/common/token_util.*        Token 生成 / base64url / 周期边界
rtunnel/relay/admin/backend/admin_db_accounts.cpp   账号与 Token 存取
rtunnel/relay/admin/backend/api_accounts.cpp        管理 API
rtunnel/relay/admin/frontend/admin_accounts.js      接入账号页
```

协议扩展在 `rtnp/frame.hpp`（`TOKEN_*` 错误码）与 `common/tunnel_link.cpp`（auth_type=2、
AUTH_OK 尾部、致命认证错误不再重连）。

## 10.2 客户端用法

```bash
# 只带 Token，用户名由 relay 从 Token 解析后回填
rtunnel-client --token rtk_xxx --socks5 127.0.0.1:1080

# 也可以两者都给，但必须一致，否则 AUTH_FAIL
rtunnel-client --user alice --token rtk_xxx --socks5 127.0.0.1:1080
```

识别规则：`rtk_` 前缀走 `auth_type=2`，其他按静态凭据（`auth_type=1`）。
静态凭据仍然可用，但同名账号被停用后同样会被拒绝。

## 10.3 生效与落库

```
创建 / 修改 / 轮换 / 删除 Token  → 立即推送全量策略到 relay
relay 侧限流、并发、流量计量      → 全在事件循环线程，无锁
用量快照                          → 每 token.usage_flush_ms（默认 5s）写回 SQLite
周期重置                          → relay 5s 扫描 + 写回时 period_start_ms 乐观锁
```

## 10.4 运维检查

```bash
# 某个账号当前能用什么
权限页 →「有效权限」，或
curl -b cookie 'http://127.0.0.1:9443/api/v1/client-accounts/1/effective'

# Token 用量
sqlite3 admin.db "select id,name,used_bytes,quota_bytes,last_used_ip from client_tokens;"

# 客户端被拒原因
relay.log 搜 "token auth rejected" / "client auth rejected"
客户端日志搜 "auth failed: code="
  12 过期  13 停用/无效  14 配额用完  15 频率超限  16 并发超限
```

## 10.5 未做的部分

```
旧凭据导入
  旧 --client-users 的 token 是任意字符串，客户端会按 auth_type=1 发送；
  若导入后仍要可用，需要让 relay 对 type=1 也查 Token 哈希，等于让两套凭据语义混在一起。
  当前选择：静态凭据与账号 Token 并行存在，新部署直接用 Token，旧凭据保留到迁移完成。

Token 级设备白名单
  v1 按设计不做；需要多租户共用账号时再加 token_devices 表。

多 relay 实例
  策略与用量都在进程内，SQLite 单写，仍只支持单实例。
```
