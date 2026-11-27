// 审计动作的中文名：后端记的是 account.login 这类稳定键，界面给人看的是中文。
// 没收录的按前缀兜底（见 actionLabel），原始键仍然显示在第二行，方便对日志。
const EXACT = {
  'setup': '初始化平台',
  'account.create': '创建账号',
  'account.delete': '删除账号',
  'account.login': '登录',
  'account.logout_all': '全部会话下线',
  'account.password': '改密码',
  'account.role': '改角色',
  'account.status': '启用/停用账号',
  'admin_token.create': '新建 API Token',
  'admin_token.revoke': '吊销 API Token',
  'admin_token.rotate': '轮换 API Token',
  'admin_token.update': '修改 API Token',
  'agent.caps': '改设备能力',
  'agent.diag': '设备诊断',
  'agent.power': '设备关机/重启',
  'agent.proc.signal': '发信号给进程',
  'agents.kick': '踢设备下线',
  'ai.key.set': '设置模型密钥',
  'ai.key.clear': '清除模型密钥',
  'ai.profile': '改模型配置',
  'ai.session.delete': '删除对话记录',
  'alerts.test': '发测试告警',
  'billing.price.update': '改计费价格',
  'billing.statement.generate': '生成账单',
  'billing.statement.status': '改账单状态',
  'certs.issue': '签发证书',
  'certs.revoke': '吊销证书',
  'certs.sign': '签发客户端证书',
  'client_account.create': '创建接入账号',
  'client_account.delete': '删除接入账号',
  'client_account.kick': '踢客户端下线',
  'client_account.update': '修改接入账号',
  'client_token.create': '新建接入令牌',
  'client_token.delete': '删除接入令牌',
  'client_token.rotate': '轮换接入令牌',
  'client_token.update': '修改接入令牌',
  'deploy.host.add': '登记部署主机',
  'deploy.host.delete': '删除部署主机',
  'deploy.host.update': '修改部署主机',
  'deploy.job.cancel': '取消部署',
  'deploy.job.create': '发起部署',
  'deploy.probe': '测试主机连接',
  'deploy.scan': '扫描网段',
  'distribution.cancel': '取消分发',
  'distribution.create': '创建分发任务',
  'distribution.extend': '延长分发截止',
  'distribution.retry': '重试分发',
  'distribution.verify_diff': '分发校验（不一致）',
  'distribution.verify_same': '分发校验（一致）',
  'files.delete': '删除文件',
  'files.edit': '编辑文件',
  'files.mkdir': '新建目录',
  'files.move': '移动文件',
  'files.upload': '上传文件',
  'metrics.series_config': '改指标采样',
  'ota.create': '创建升级任务',
  'permission.client.groups': '改客户端权限组',
  'permission.device.update': '改设备授权',
  'permission.dict.port.add': '新增端口字典',
  'permission.dict.tty.add': '新增串口字典',
  'permission.group.create': '新建权限组',
  'permission.group.delete': '删除权限组',
  'permission.group.members': '改权限组成员',
  'permission.group.update': '修改权限组',
  'permission.resource.update': '改账号资源授权',
  'permission.subject.groups': '改授权对象分组',
  'permission.user.groups': '改账号权限组',
  'perm.denied': '越权请求被拒',
  'playbook.create': '新建自愈规则',
  'playbook.delete': '删除自愈规则',
  'playbook.enable': '启停自愈规则',
  'playbook.run': '手动执行自愈',
  'playbook.update': '修改自愈规则',
  'proxy.open': '打开通道',
  'relay.dict.add': '新增中继地址',
  'relay.dict.batch': '批量导入中继地址',
  'relay.dict.delete': '删除中继地址',
  'relay.dict.update': '修改中继地址',
  'relay.list.set': '保存下发列表',
  'relay.sync.cancel': '取消中继同步',
  'relay.sync.create': '创建中继同步',
  'relay.sync.retry': '重试中继同步',
  'settings.update': '改系统设置',
  'streams.kick': '断开会话',
  'tenant.create': '新建租户',
  'tenant.update': '修改租户',
  'terminal.open': '打开终端',
  'tty.config': '改串口配置',
  'tty.debug_open': '打开调试串口',
  'tty.delete': '删除串口',
  'tty.monitor': '监听串口',
  'tty.takeover': '接管串口',
}

const PREFIX = [
  ['permission.dict.', '字典'],
  ['permission.', '权限'],
  ['relay.sync.', '中继同步'],
  ['relay.', '中继'],
  ['deploy.', '部署'],
  ['distribution.', '分发'],
  ['files.', '文件'],
  ['playbook.', '自愈'],
  ['billing.', '计费'],
  ['certs.', '证书'],
  ['tenant.', '租户'],
  ['tty.', '串口'],
  ['client_', '接入'],
  ['account.', '账号'],
  ['admin_token.', 'API Token'],
  ['ai.', '助手'],
]

export function actionLabel(code) {
  if (!code) return '—'
  if (EXACT[code]) return EXACT[code]
  for (const [p, name] of PREFIX) {
    if (code.startsWith(p)) return name + ' · ' + code.slice(p.length)
  }
  return code
}

const RESULT = {
  ok: { text: '成功', type: 'success' },
  denied: { text: '拒绝', type: 'warning' },
  error: { text: '错误', type: 'danger' },
}

export function resultLabel(v) {
  return RESULT[v] || { text: v || '—', type: 'info' }
}

const ACTOR = { user: '账号', client: '接入账号', token: '令牌', ai: '助手', anonymous: '匿名' }

export function actorLabel(t) {
  return ACTOR[t] || t || ''
}
