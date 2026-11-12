// 导航与角色可见性：路由守卫与侧栏共用一份
export const NAV = [
  { path: '/overview', title: '概览', icon: 'Odometer', roles: ['viewer', 'operator', 'manager', 'admin'] },
  { path: '/agents', title: '设备 C', icon: 'Monitor', roles: ['viewer', 'operator', 'manager', 'admin'] },
  { path: '/permission', title: '权限', icon: 'Lock', roles: ['operator', 'manager', 'admin'] },
  { path: '/clients', title: '客户端 A', icon: 'Connection', roles: ['manager', 'admin'] },
  { path: '/access', title: '接入账号', icon: 'Key', roles: ['operator', 'manager', 'admin'] },
  { path: '/streams', title: '会话', icon: 'Sort', roles: ['operator', 'manager', 'admin'] },
  { path: '/certs', title: '证书', icon: 'Document', roles: ['admin'] },
  { path: '/accounts', title: '账号', icon: 'User', roles: ['manager', 'admin'] },
  { path: '/files', title: '文件', icon: 'FolderOpened', roles: ['operator', 'manager', 'admin'] },
  { path: '/tasks', title: '任务', icon: 'List', roles: ['manager', 'admin'] },
  { path: '/audit', title: '审计', icon: 'Tickets', roles: ['manager', 'admin'] },
  { path: '/settings', title: '设置', icon: 'Setting', roles: ['admin'] },
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
