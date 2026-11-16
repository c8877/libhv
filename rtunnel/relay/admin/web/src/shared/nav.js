// 导航与角色可见性：路由守卫与侧栏共用一份
const NAV = [
  { path: '/overview', icon: 'Odometer', roles: ['viewer', 'operator', 'manager', 'admin'] },
  { path: '/agents', icon: 'Monitor', roles: ['viewer', 'operator', 'manager', 'admin'] },
  { path: '/permission', icon: 'Lock', roles: ['operator', 'manager', 'admin'] },
  { path: '/clients', icon: 'Connection', roles: ['manager', 'admin'] },
  { path: '/access', icon: 'Key', roles: ['operator', 'manager', 'admin'] },
  { path: '/streams', icon: 'Sort', roles: ['operator', 'manager', 'admin'] },
  { path: '/certs', icon: 'Document', roles: ['admin'] },
  { path: '/accounts', icon: 'User', roles: ['manager', 'admin'] },
  { path: '/files', icon: 'FolderOpened', roles: ['operator', 'manager', 'admin'] },
  { path: '/tasks', icon: 'List', roles: ['manager', 'admin'] },
  { path: '/ota', icon: 'Refresh', roles: ['manager', 'admin'] },
  { path: '/audit', icon: 'Tickets', roles: ['manager', 'admin'] },
  { path: '/settings', icon: 'Setting', roles: ['admin'] },
]

export function navFor(role) {
  return NAV.filter((n) => n.roles.includes(role))
}

export function canVisit(path, role) {
  if (path === '/profile') return true
  const item = NAV.find((n) => path === n.path || path.startsWith(n.path + '/'))
  if (!item) return true
  return item.roles.includes(role)
}
