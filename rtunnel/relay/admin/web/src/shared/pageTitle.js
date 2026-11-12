const TITLES = {
  '/login': '登录',
  '/overview': '概览',
  '/agents': '设备 C',
  '/permission': '权限',
  '/clients': '客户端 A',
  '/access': '接入账号',
  '/streams': '会话',
  '/certs': '证书',
  '/accounts': '账号',
  '/files': '文件',
  '/distribute': '分发文件',
  '/tasks': '任务',
  '/audit': '审计',
  '/settings': '设置',
  '/profile': '个人设置',
}

const PREFIX = [
  ['/agents/', '设备详情'],
  ['/terminal/', '终端'],
  ['/access/', '接入账号详情'],
  ['/tasks/', '任务详情'],
]

export function titleFor(path) {
  if (TITLES[path]) return TITLES[path]
  for (const [prefix, title] of PREFIX) {
    if (path.startsWith(prefix)) return title
  }
  return ''
}
