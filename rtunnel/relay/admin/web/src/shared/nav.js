// 导航分组与角色可见性：路由守卫与侧栏共用一份。
// 组标题在 AppLayout 中渲染为可折叠的 el-sub-menu，空组由 navFor() 过滤掉。
const ALL = ['viewer', 'operator', 'manager', 'admin']

export const NAV_GROUPS = [
  {
    key: 'monitor',
    label: '监控',
    items: [
      { path: '/overview', icon: 'Odometer', perm: 'device.view', roles: ALL },
      { path: '/agents', icon: 'Monitor', label: '设备', perm: 'device.view', roles: ALL },
      { path: '/streams', icon: 'Sort', perm: 'device.view', roles: ['operator', 'manager', 'admin'] },
    ],
  },
  {
    key: 'ops',
    label: '运维',
    items: [
      { path: '/files', icon: 'FolderOpened', perm: 'file.read', roles: ['operator', 'manager', 'admin'] },
      { path: '/distribute', icon: 'Upload', label: '分发', perm: 'dist.write', roles: ['manager', 'admin'] },
      { path: '/tasks', icon: 'List', perm: 'dist.read', roles: ['manager', 'admin'] },
      { path: '/ota', icon: 'Refresh', perm: 'dist.write', roles: ['manager', 'admin'] },
    ],
  },
  {
    key: 'perm',
    label: '权限',
    items: [
      { path: '/groups', icon: 'UserFilled', label: '权限组', perm: 'dict.read', roles: ['manager', 'admin'] },
      { path: '/subjects', icon: 'Avatar', label: '授权对象', perm: 'dict.read', roles: ['manager', 'admin'] },
      { path: '/dicts', icon: 'Collection', label: '字典库', perm: 'dict.read', roles: ['operator', 'manager', 'admin'] },
      { path: '/access', icon: 'Key', perm: 'account.read', roles: ['operator', 'manager', 'admin'] },
      { path: '/clients', icon: 'Connection', label: '客户端', perm: 'device.view', roles: ['manager', 'admin'] },
      { path: '/accounts', icon: 'User', perm: 'account.read', roles: ['manager', 'admin'] },
    ],
  },
  {
    key: 'openapi',
    label: '开放接口',
    items: [
      { path: '/tokens', icon: 'Postcard', perm: 'token.read', roles: ['manager', 'admin'] },
      { path: '/docs', icon: 'Document', perm: 'auth', roles: ALL },
    ],
  },
  {
    key: 'system',
    label: '系统',
    items: [
      { path: '/certs', icon: 'Document', perm: 'cert.read', roles: ['admin'] },
      { path: '/audit', icon: 'Tickets', perm: 'audit.read', roles: ['manager', 'admin'] },
      { path: '/settings', icon: 'Setting', perm: 'settings.read', roles: ['admin'] },
    ],
  },
]

// 返回 [{ key, label, items }]，优先按资源权限过滤，未拿到权限时退回角色
export function navFor(role, can) {
  const allow = (n) => {
    if (n.perm === 'auth') return true
    if (typeof can === 'function') return can(n.perm)
    return n.roles.includes(role)
  }
  return NAV_GROUPS
    .map((g) => ({ ...g, items: g.items.filter(allow) }))
    .filter((g) => g.items.length)
}

export function canVisit(path, role, can) {
  if (path === '/profile') return true
  for (const g of NAV_GROUPS) {
    const item = g.items.find((n) => path === n.path || (n.path !== '/' && path.startsWith(n.path + '/')))
    if (item) {
      if (item.perm === 'auth') return true
      if (typeof can === 'function') return can(item.perm)
      return item.roles.includes(role)
    }
  }
  return true
}
