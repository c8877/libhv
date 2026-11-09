# 权限与分组详细设计

状态：设计稿  
范围：管理台权限页、A 侧客户端 ACL、agent 资源分组  
替代：原 `docs/acl.md`

## 1. 目标

把权限管理从“ACL 文件 + 平铺 devid”升级为：

```
客户端分组  ── ACL ──  设备分组
     │                    │
   client 用户           devid
```

- 管理台新增「权限」菜单和权限管理页面。
- 权限页分两个 Tab：
  - 客户端分组
  - 设备分组（agent 分组）
- 分组表格行内展开表单，直接维护成员和 ACL，不再跳详情页。
- ACL 以“客户端分组 → 设备分组”的矩阵配置，支持按设备功能项授权。
- 一个 client 可属于多个客户端分组。
- 一个 devid 可属于多个设备分组。
- 权限通过分组复用。

## 2. 术语

| 名称 | 说明 |
|---|---|
| 客户端分组 | 一组 A 侧 client 用户，权限配置的主体 |
| 设备分组 | 一组 agent/devid，权限配置的资源 |
| ACL 条目 | 客户端分组对设备分组的一组能力授权 |
| 能力上限 | 设备分组声明的最大可开放能力 |
| 有效权限 | ACL ∩ 设备分组上限 ∩ agent 实际能力 ∩ 角色上限 |

## 3. 总体模型

```
client 用户
   │ 多对多
   ▼
客户端分组 ───── ACL 条目 ─────▶ 设备分组
                                  │ 多对多
                                  ▼
                                 devid
```

ACL 条目的粒度：

```
客户端分组 × 设备分组
  tcp_ports
  shell
  tty
  file
  sysinfo
  metrics
  config
  web
```

## 4. 权限计算

```
client 的客户端分组 = C1, C2
设备分组集合 G = C1.ACL ∪ C2.ACL
devid 集合 D = G 中所有成员并集

对每个 devid d：
  grants   = 所有包含 d 的设备分组对应的 ACL 能力并集
  ceiling  = 设备分组能力上限
  actual   = agent caps
  effective = grants ∩ ceiling ∩ actual
```

默认拒绝。

未加入任何客户端分组的 client，按旧 ACL/allow-all 策略兼容；加入后以分组为准。

## 5. 数据模型

```sql
CREATE TABLE client_groups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    description TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL
);

CREATE TABLE client_group_members (
    group_id INTEGER NOT NULL,
    username TEXT NOT NULL,
    added_ms INTEGER NOT NULL,
    PRIMARY KEY (group_id, username)
);

CREATE TABLE agent_groups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    description TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL
);

CREATE TABLE agent_group_members (
    group_id INTEGER NOT NULL,
    devid TEXT NOT NULL,
    added_ms INTEGER NOT NULL,
    PRIMARY KEY (group_id, devid)
);

CREATE TABLE agent_group_caps (
    group_id INTEGER NOT NULL,
    cap TEXT NOT NULL,
    value TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (group_id, cap)
);

CREATE TABLE client_group_acl (
    client_group_id INTEGER NOT NULL,
    agent_group_id INTEGER NOT NULL,
    tcp_ports TEXT NOT NULL DEFAULT '',
    allow_shell INTEGER NOT NULL DEFAULT 0,
    tty TEXT NOT NULL DEFAULT '',
    file TEXT NOT NULL DEFAULT 'none',
    sysinfo INTEGER NOT NULL DEFAULT 0,
    metrics INTEGER NOT NULL DEFAULT 0,
    config INTEGER NOT NULL DEFAULT 0,
    web INTEGER NOT NULL DEFAULT 0,
    updated_ms INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (client_group_id, agent_group_id)
);
```

### 5.1 能力字典

TCP 端口和 TTY 不再用自由文本，改为全局字典 + 关联表。

```sql
CREATE TABLE port_dict (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    port_start INTEGER NOT NULL,
    port_end INTEGER NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    enabled INTEGER NOT NULL DEFAULT 1,
    created_by TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    UNIQUE(port_start, port_end)
);

CREATE TABLE tty_dict (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    pattern TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    kind TEXT NOT NULL DEFAULT 'serial',
    enabled INTEGER NOT NULL DEFAULT 1,
    created_by TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL
);
```

设备分组能力上限：

```sql
CREATE TABLE agent_group_ports (
    group_id INTEGER NOT NULL,
    port_id INTEGER NOT NULL,
    PRIMARY KEY(group_id, port_id)
);

CREATE TABLE agent_group_ttys (
    group_id INTEGER NOT NULL,
    tty_id INTEGER NOT NULL,
    PRIMARY KEY(group_id, tty_id)
);
```

客户端分组 ACL：

```sql
CREATE TABLE client_acl_ports (
    client_group_id INTEGER NOT NULL,
    agent_group_id INTEGER NOT NULL,
    port_id INTEGER NOT NULL,
    PRIMARY KEY(client_group_id, agent_group_id, port_id)
);

CREATE TABLE client_acl_ttys (
    client_group_id INTEGER NOT NULL,
    agent_group_id INTEGER NOT NULL,
    tty_id INTEGER NOT NULL,
    PRIMARY KEY(client_group_id, agent_group_id, tty_id)
);
```

说明：

- 端口字典项可以是单端口或范围：`22`、`8000-8100`。
- TTY 字典项可以是具体路径或模式：`ttyS0`、`ttyUSB*`。
- `client_group_acl.tcp_ports` / `tty` 与 `dev_group_caps` 的文本值保留为可读快照；
  运行期以关联表为准，保存时同步写入。启动时按文本重建一次，兼容旧库。
- 字典项禁用后立即从有效权限中移除，已建立会话不打断。

说明：

- `agent_group_caps` 是设备分组的**能力上限**，不是最终授权。
- `client_group_acl` 是具体授权，可以在上限内进一步收窄。
- 管理台账号范围继续使用 `agent_groups`，在账号页维护，不在权限页 Tab 内。

## 6. 迁移

当前已有原型表：

```
dev_groups / dev_group_members / dev_group_caps / subject_groups
```

迁移：

1. `dev_groups` → `agent_groups`
2. `dev_group_members` → `agent_group_members`
3. `dev_group_caps` → `agent_group_caps`
4. `subject_groups` 中 `client` 记录：
   - 按用户名生成 `client_groups`；
   - 加入 `client_group_members`；
   - 原分组成员生成 `client_group_acl` 条目。
5. `subject_groups` 中 `admin` 记录保留为管理台账号范围。
6. 旧 ACL 文件导入：
   - 每个 `user=devid` 生成一个客户端分组；
   - 生成一个设备分组；
   - 建立 ACL 条目，能力默认全开。

## 7. 后端 API

### 客户端分组

```text
GET    /api/v1/permission/client-groups
POST   /api/v1/permission/client-groups
GET    /api/v1/permission/client-groups/:id
PUT    /api/v1/permission/client-groups/:id
DELETE /api/v1/permission/client-groups/:id

POST   /api/v1/permission/client-groups/:id/users
DELETE /api/v1/permission/client-groups/:id/users/:username

GET    /api/v1/permission/client-groups/:id/acl
PUT    /api/v1/permission/client-groups/:id/acl
```

### 设备分组

```text
GET    /api/v1/permission/agent-groups
POST   /api/v1/permission/agent-groups
GET    /api/v1/permission/agent-groups/:id
PUT    /api/v1/permission/agent-groups/:id
DELETE /api/v1/permission/agent-groups/:id

POST   /api/v1/permission/agent-groups/:id/devices
DELETE /api/v1/permission/agent-groups/:id/devices/:devid

PUT    /api/v1/permission/agent-groups/:id/caps

GET    /api/v1/permission/agent-groups/:id/candidates?q=&online=1
```

### 能力字典

```text
GET    /api/v1/permission/dict/ports?q=&enabled=1
POST   /api/v1/permission/dict/ports
POST   /api/v1/permission/dict/ports/batch
PUT    /api/v1/permission/dict/ports/:id
DELETE /api/v1/permission/dict/ports/:id
POST   /api/v1/permission/dict/ports/batch-delete

GET    /api/v1/permission/dict/ttys?q=&enabled=1
POST   /api/v1/permission/dict/ttys
POST   /api/v1/permission/dict/ttys/batch
PUT    /api/v1/permission/dict/ttys/:id
DELETE /api/v1/permission/dict/ttys/:id
POST   /api/v1/permission/dict/ttys/batch-delete
```

新增端口：

```json
{ "port_start": 8080, "port_end": 8080, "name": "Web 调试", "description": "" }
```

新增 TTY：

```json
{ "pattern": "ttyUSB*", "name": "USB 串口", "kind": "serial" }
```

重复新增返回已有条目 id，幂等。

### 预览

```text
GET /api/v1/permission/effective?client=alice
```

返回：

```json
{
  "client": "alice",
  "groups": ["研发", "调试"],
  "devices": ["dev1", "dev2"],
  "caps": {
    "tcp_ports": "22,80,8000-8100",
    "shell": 1,
    "tty": "ttyS*",
    "file": "ro"
  },
  "clipped": [
    "dev1: 设备组不允许 8000-8100"
  ]
}
```

## 8. 权限执行

| 位置 | 检查 |
|---|---|
| OPEN | devid 在有效设备集合内，所选端口字典项在 ACL ∩ 上限内 |
| OPEN_SHELL | ACL allow_shell + 设备组上限 + shell-users + agent caps |
| OPEN_TTY | ACL 所选 TTY 字典项 + 设备组上限 + agent tty 配置 |
| 管理台设备列表 | 管理账号的 agent group 范围 |
| 文件/分发 | 设备范围 + file 能力 |
| SYSINFO | sysinfo 能力 |
| 指标 | metrics 能力 |
| agent 配置 | config 能力 + 管理角色 |

## 9. 审计

记录：

- 客户端分组创建/修改/删除
- 客户端成员加入/移出
- 设备分组创建/修改/删除
- devid 加入/移出
- ACL 条目新增/修改/删除
- 设备分组能力上限修改

## 10. 约束

- 分组名在同类分组内唯一。
- 删除客户端分组：删除其 ACL 和成员，需要确认。
- 删除设备分组：如果被 ACL 引用，拒绝删除。
- 一个 client 可属于多个客户端分组。
- 一个 devid 可属于多个设备分组。
- ACL 同一对分组只有一条。
- 端口格式：`22,80,8000-8100,*`；非法条目整行保存失败。
- 第一版不做显式 deny，权限取并集。

## 11. 页面

详细页面设计见：

```
docs/admin/permission.md
```
