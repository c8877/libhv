# 权限与分组开发规划

对应设计：`docs/permission.md`  
页面设计：`docs/admin/permission.md`

当前代码里已有原型：

```
dev_groups / dev_group_members / dev_group_caps / subject_groups
```

原型是“单层设备分组 + 账号关联”，还没有拆成客户端分组/设备分组和 ACL 矩阵。  
下面按新模型分阶段改造。

## 阶段 0：设计文档

- [x] `docs/permission.md`
- [x] `docs/admin/permission.md`
- [x] 旧 `docs/acl.md` 重命名

## 阶段 1：数据模型

- [ ] `client_groups`
- [ ] `client_group_members`
- [ ] `agent_groups`
- [ ] `agent_group_members`
- [ ] `agent_group_caps`
- [ ] `client_group_acl`
- [ ] 原型表迁移
- [ ] 旧 ACL 导入客户端分组

验收：

- 一个 client 可加入多个客户端分组；
- 一个 devid 可加入多个设备分组；
- 客户端分组可配置多个设备分组 ACL；
- ACL 能力能正确回显。

## 阶段 2：权限页面

- [ ] 侧边栏「权限」菜单
- [ ] `#/permission` 路由
- [ ] 客户端分组 Tab
- [ ] 设备分组 Tab
- [ ] 表格行内展开分组表单
- [ ] 客户端成员添加/移除
- [ ] 设备成员添加/移除
- [ ] ACL 矩阵编辑
- [ ] 设备分组能力上限编辑
- [ ] 行内保存/取消/错误
- [ ] 空状态与未保存提示
- [ ] 旧 ACL 导入入口

验收：

- 不跳转页面即可完成分组创建、成员维护、ACL 配置；
- 同一时间只展开一行；
- 保存失败不关闭表单；
- 删除被引用设备分组被拒绝。

## 阶段 3：运行时

- [ ] relay 加载客户端分组 ACL
- [ ] OPEN 按客户端分组 → 设备分组 → devid 检查
- [ ] 端口/Shell/TTY 检查
- [ ] 设备分组能力上限裁剪
- [ ] agent caps 最终裁剪
- [ ] 分组变更热更新
- [ ] 管理台账号仍按设备分组范围

验收：

- client 只能访问 ACL 允许的设备和功能；
- 多分组权限并集；
- ACL 不能突破设备分组上限和 agent caps；
- 已建立会话不受新 ACL 影响。

## 阶段 4：收口

- [ ] 旧 ACL 文件只保留导入/导出
- [ ] `#/acl` 重定向到 `#/permission`
- [ ] 有效权限预览页面
- [ ] 审计覆盖全部权限变更
- [ ] 删除原型表兼容代码
- [ ] 运维说明
