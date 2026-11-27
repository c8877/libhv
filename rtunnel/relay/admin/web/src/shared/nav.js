// 导航分组与角色可见性：路由守卫与侧栏共用一份。
// 这里只写“结构 + 图标 + 权限”，显示名统一取 router 的 meta.title，
// 避免同一个名字在两处维护、改一处漏一处。
export const NAV_GROUPS = [
  {
    // 首页两项不折叠，但同样给一个组标题：否则它们和下面的组标题不是同级字号
    key: 'home',
    flat: true,
    label: '工作台',
    items: [
      { path: '/', icon: 'ChatDotRound', perm: 'ai.chat' },
      { path: '/overview', icon: 'Odometer', perm: 'device.view' },
    ],
  },
  {
    // 当前连着谁：设备、终端侧客户端、会话、中继拓扑
    key: 'device',
    label: '设备与链路',
    items: [
      { path: '/agents', icon: 'Monitor', perm: 'device.view' },
      { path: '/clients', icon: 'Connection', perm: 'device.view' },
      { path: '/streams', icon: 'Sort', perm: 'device.view' },
      { path: '/relay-list', icon: 'Share', perm: 'relay.read' },
    ],
  },
  {
    // 文件仓库 → 分发 / 升级：同一份包的三条出路
    key: 'artifacts',
    label: '文件与升级',
    items: [
      { path: '/files', icon: 'FolderOpened', perm: 'repo.read' },
      { path: '/distribute', icon: 'Upload', perm: 'dist.write' },
      { path: '/tasks', icon: 'List', perm: 'dist.read' },
      { path: '/ota', icon: 'Refresh', perm: ['dist.write', 'device.view', 'repo.read'] },
    ],
  },
  {
    // 把组件装上去，以及装上去之后自动做什么
    key: 'automation',
    label: '接入与自动化',
    items: [
      { path: '/deploy', icon: 'UploadFilled', perm: 'deploy.read' },
      { path: '/operations', icon: 'Tickets', perm: 'device.exec' },
      { path: '/playbooks', icon: 'MagicStick', perm: 'playbook.read' },
    ],
  },
  {
    // 谁在用、谁管谁
    key: 'org',
    label: '组织与权限',
    items: [
      { path: '/tenants', icon: 'OfficeBuilding', perm: 'tenant.read' },
      { path: '/accounts', icon: 'User', perm: 'account.read' },
      { path: '/access', icon: 'Key', perm: 'account.read' },
      { path: '/groups', icon: 'UserFilled', perm: 'group.read' },
      { path: '/subjects', icon: 'Avatar', perm: 'group.read' },
      { path: '/dicts', icon: 'Collection', perm: 'dict.read' },
    ],
  },
  {
    key: 'money',
    label: '用量与费用',
    items: [
      { path: '/usage', icon: 'TrendCharts', perm: 'usage.read' },
      { path: '/billing', icon: 'Wallet', perm: 'billing.read' },
    ],
  },
  {
    key: 'openapi',
    label: '开放接口',
    items: [
      { path: '/tokens', icon: 'Postcard', perm: 'token.read' },
      { path: '/docs', icon: 'Document', perm: 'auth' },
      { path: '/guide', icon: 'Reading', perm: 'auth' },
    ],
  },
  {
    key: 'system',
    label: '系统',
    items: [
      { path: '/alerts', icon: 'Bell', perm: 'alert.read' },
      { path: '/certs', icon: 'Lock', perm: 'cert.read' },
      { path: '/audit', icon: 'Memo', perm: 'audit.read' },
      { path: '/settings', icon: 'Setting', perm: 'settings.read' },
    ],
  },
]

// perm 可以是单个键或键数组；数组表示同时需要。菜单只按资源权限过滤。
function allowItem(item, can) {
  const perms = Array.isArray(item.perm) ? item.perm : [item.perm]
  if (perms.length === 1 && perms[0] === 'auth') return true
  return perms.every((p) => can(p))
}

// 返回 [{ key, label, items }]，空组由调用方过滤
export function navFor(can) {
  return NAV_GROUPS
    .map((g) => ({ ...g, items: g.items.filter((n) => allowItem(n, can)) }))
    .filter((g) => g.items.length)
}

export function canVisit(path, can) {
  if (path === '/profile') return true
  for (const g of NAV_GROUPS) {
    const item = g.items.find((n) => path === n.path || (n.path !== '/' && path.startsWith(n.path + '/')))
    if (item) return allowItem(item, can)
  }
  return true
}
