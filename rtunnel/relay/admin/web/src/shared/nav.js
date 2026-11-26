// 导航分组与角色可见性：路由守卫与侧栏共用一份。
// 组标题在 AppLayout 中渲染为可折叠的 el-sub-menu，空组由 navFor() 过滤掉。
export const NAV_GROUPS = [
  {
    key: 'monitor',
    label: '监控',
    items: [
      { path: '/overview', icon: 'Odometer', perm: 'device.view' },
      { path: '/agents', icon: 'Monitor', label: '设备', perm: 'device.view' },
      { path: '/streams', icon: 'Sort', perm: 'device.view' },
      { path: '/usage', icon: 'TrendCharts', label: '用量', perm: 'usage.read' },
      { path: '/tenants', icon: 'OfficeBuilding', label: '租户', perm: 'tenant.read' },
      { path: '/billing', icon: 'Wallet', label: '费用', perm: 'billing.read' },
    ],
  },
  {
    key: 'ops',
    label: '运维',
    items: [
      { path: '/files', icon: 'FolderOpened', perm: 'repo.read' },
      { path: '/distribute', icon: 'Upload', label: '分发', perm: 'dist.write' },
      { path: '/tasks', icon: 'List', perm: 'dist.read' },
      { path: '/operations', icon: 'Tickets', label: '作业', perm: 'device.exec' },
      { path: '/ota', icon: 'Refresh', perm: ['dist.write', 'device.view', 'repo.read'] },
      { path: '/relay-list', icon: 'Connection', perm: 'relay.read' },
    ],
  },
  {
    key: 'perm',
    label: '权限',
    items: [
      { path: '/groups', icon: 'UserFilled', label: '权限组', perm: 'group.read' },
      { path: '/subjects', icon: 'Avatar', label: '授权对象', perm: 'group.read' },
      { path: '/dicts', icon: 'Collection', label: '字典库', perm: 'dict.read' },
      { path: '/access', icon: 'Key', perm: 'account.read' },
      { path: '/clients', icon: 'Connection', label: '客户端', perm: 'device.view' },
      { path: '/accounts', icon: 'User', perm: 'account.read' },
    ],
  },
  {
    key: 'openapi',
    label: '开放接口',
    items: [
      { path: '/tokens', icon: 'Postcard', perm: 'token.read' },
      { path: '/docs', icon: 'Document', perm: 'auth' },
    ],
  },
  {
    key: 'system',
    label: '系统',
    items: [
      { path: '/certs', icon: 'Document', perm: 'cert.read' },
      { path: '/audit', icon: 'Tickets', perm: 'audit.read' },
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
