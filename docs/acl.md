# ACL 与设备分组详细设计

状态：设计稿  
范围：rtunnel relay / admin / A 侧客户端  
目标：把当前 `user=devid` 平铺 ACL 升级为“设备分组 + 能力策略 + 账号授权”的可复用模型。

## 1. 背景

当前实现存在以下限制：

- ACL 为单文件 `user=devid[,devid|*]`，设备多时规则重复、无法复用。
- 管理台账号只有全局角色，没有设备可见范围。
- A 侧客户端权限和 agent 能力、管理台角色分散在三处，难以统一排查。
- 无法表达“只允许某组设备的某几个端口 / Shell / TTY / 文件能力”。

## 2. 目标与非目标

### 目标

1. 以“设备分组”为管理单位，一个 devid 可属于多个分组。
2. 分组同时承载设备集合与能力策略，账号加入分组即获得授权。
3. 同一套分组同时服务：
   - A 侧客户端用户 ACL；
   - 管理台账号的设备可见范围与操作范围。
4. 支持按设备功能项授权：TCP 端口、Shell、TTY、文件、SYSINFO、指标、agent 配置。
5. 提供有效权限预览，能区分“分组未授权”和“agent 未开放”。
6. 保留旧 ACL 导入，迁移期可回退。

### 非目标

- 不引入嵌套分组。
- 第一版不实现显式 deny；冲突按并集处理。
- 不改变 agent 的硬能力边界。分组不能突破 `agent caps`。

## 3. 统一授权模型

```
管理台账号 ─┐
            ├─ subject_groups ─ 设备分组 ─ group_members ─ devid
A 侧客户端 ─┘                     │
                                  └─ group_caps
```

有效权限：

```
设备范围 = 账号所在分组包含的 devid 并集
能力范围 = 账号所在分组的能力并集
最终能力 = 能力范围 ∩ agent 能力 ∩ 角色上限
```

默认拒绝。`scope_all` 账号不受分组设备范围限制，仅 admin 可设置。

## 4. 核心对象

### 4.1 设备分组

| 字段 | 说明 |
|---|---|
| `id` | 主键 |
| `name` | 唯一名称，如“产线A” |
| `description` | 描述 |
| `created_ms` | 创建时间 |
| `updated_ms` | 更新时间 |

### 4.2 分组成员

`(group_id, devid)` 唯一。允许加入尚未上线的 devid。

### 4.3 分组能力

按能力行存储，便于扩展：

| cap | value | 说明 |
|---|---|---|
| `tcp_ports` | `22,80,8000-8100,*` | 允许 OPEN 的端口 |
| `shell` | `0/1` | 允许 OPEN_SHELL |
| `tty` | `ttyS*,ttyUSB0` | 允许访问的串口 |
| `file` | `none/ro/rw` | 文件查看/上传/分发 |
| `sysinfo` | `0/1` | 查看设备快照 |
| `metrics` | `0/1` | 采样配置与查询 |
| `config` | `0/1` | 修改 agent 能力配置 |
| `web` | `0/1` | 打开设备 Web 端口 |

端口能力仍需与 agent 的 `--allow` 做交集。

### 4.4 账号关联

```
subject_type = admin | client
subject_id   = 用户名
```

证书 SAN 与用户名一致性由 mTLS 层保证，分组不保存证书。

## 5. 权限计算

```text
groups(subject)
  -> members  = union(devid)
  -> caps     = union(cap values)
  -> effective = caps ∩ agent.caps ∩ role.caps
```

端口做集合并集，Shell/TTY 做布尔或模式并集。  
多分组冲突取并集，不做 deny。

## 6. 执行点

| 位置 | 检查 |
|---|---|
| A 侧 OPEN | devid 在账号分组内，且端口被允许 |
| OPEN_SHELL | 分组 shell + role + `--shell-users` + agent caps |
| TTY | 分组 tty 模式 + agent tty 配置 |
| 管理台设备列表 | 仅显示账号分组内设备 |
| 设备详情 / 文件 / 分发 | 设备范围 + file 能力 |
| SYSINFO | sysinfo 能力 |
| 指标配置/查询 | metrics 能力 |
| agent 能力修改 | config 能力 + manager/admin |
| 账号 / 证书 / 系统设置 | 仍按全局角色 |

## 7. API

```text
GET    /api/v1/groups
POST   /api/v1/groups
GET    /api/v1/groups/:id
PUT    /api/v1/groups/:id
DELETE /api/v1/groups/:id

POST   /api/v1/groups/:id/devices
DELETE /api/v1/groups/:id/devices/:devid
PUT    /api/v1/groups/:id/caps

POST   /api/v1/groups/:id/subjects
DELETE /api/v1/groups/:id/subjects/:type/:id

GET    /api/v1/access/effective?type=admin&id=alice
GET    /api/v1/access/effective?type=client&id=ops
```

权限：

- 查看：operator+
- 分组/成员/能力修改：manager+
- 分配管理员账号 / `scope_all`：admin

## 8. 数据库

```sql
CREATE TABLE dev_groups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    description TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL
);

CREATE TABLE dev_group_members (
    group_id INTEGER NOT NULL,
    devid TEXT NOT NULL,
    added_ms INTEGER NOT NULL,
    PRIMARY KEY (group_id, devid)
);

CREATE TABLE dev_group_caps (
    group_id INTEGER NOT NULL,
    cap TEXT NOT NULL,
    value TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (group_id, cap)
);

CREATE TABLE subject_groups (
    subject_type TEXT NOT NULL,
    subject_id TEXT NOT NULL,
    group_id INTEGER NOT NULL,
    added_ms INTEGER NOT NULL,
    PRIMARY KEY (subject_type, subject_id, group_id)
);
```

## 9. 前端页面

### 分组列表

```
┌ 分组 ───────────────────────────── [新建分组] ┐
│ 名称      设备数  账号数  能力摘要      更新时间 │
│ 产线A     12      3      TCP 22,80 只读 10:32  │
│ 调试组    4       2      TCP * Shell    09:18  │
└────────────────────────────────────────────────┘
```

### 分组详情

```
┌ 分组：产线A ─────────────────────── [保存] [删除] ┐
│ 名称 [产线A]  描述 [一号产线]                     │
│ 设备成员 [搜索 devid] [添加设备] [批量导入]       │
│ devid  状态  其他分组  操作                       │
│ 能力策略 [x] TCP [22,80] [ ] Shell [x] TTY ...    │
│ 关联账号 admin alice / client ops                 │
└──────────────────────────────────────────────────┘
```

### 设备列表

增加：

- 分组列；
- 分组筛选；
- 批量加入分组。

### 账号

增加分组列与“编辑分组”。

### 授权预览

选择账号后展示：

- 设备列表；
- 每个设备的能力；
- 被 agent 裁剪掉的能力。

## 10. 迁移

1. 新表默认空，不影响现有行为。
2. 导入旧 ACL：
   - 每个用户生成 `legacy:<user>` 分组；
   - 设备来自旧规则；
   - 能力默认全开，再由 agent 裁剪。
3. 现有 admin 账号保留全局范围，逐步分配分组。
4. 迁移完成后以分组为准，旧 ACL 文件只保留导入/导出。

## 11. 审计

记录：

- 分组创建/更新/删除；
- 设备加入/移出；
- 能力修改；
- 账号加入/移出；
- `scope_all` 变更。

## 12. 验收

- devid 多分组，权限并集正确。
- 分组授权不能突破 agent caps。
- 管理台账号只能看到分组内设备。
- ACL 变更只影响新 OPEN，不断已有会话。
- 旧 ACL 导入后行为不变。
- 所有变更进入审计。
