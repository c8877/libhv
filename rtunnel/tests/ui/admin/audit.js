// 系统 → 审计：结果列、按结果筛选/计数、本地时间、排序、导出可用
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  let asked = ''
  const ROWS = [
    { at: '2026-01-02T03:04:05Z', actor_type: 'user', actor: 'admin#1/admin',
      action: 'deploy.host.add', target_type: 'deploy_host', target_id: '3', result: 'ok',
      detail: '10.0.0.21:22 root' },
    { at: '2026-01-02T03:05:06Z', actor_type: 'token', actor: 'ci/token7',
      action: 'perm.denied', target_type: 'perm', target_id: 'deploy.write', result: 'denied',
      detail: 'path=/api/v1/deploy/hosts need=deploy.write role=viewer' },
    { at: '2026-01-02T03:06:07Z', actor_type: 'user', actor: 'admin#1/admin',
      action: 'files.upload', target_type: 'file', target_id: '/a.tar.gz', result: 'error',
      detail: 'disk full' },
  ]

  await p.route(/\/api\/v1\/audit/, (route) => {
    asked = route.request().url()
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(ROWS) })
  })

  await p.goto(h.A + '/#/audit')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(900)

  h.check('一次取满 ring 上限', asked.includes('limit=1000'), asked)

  const cols = await p.locator('.el-table__header th').allInnerTexts()
  h.check('表头含结果与对象',
    ['时间', '操作者', '操作', '对象', '结果'].every((c) => cols.some((x) => x.includes(c))), cols)

  const body = await p.locator('.el-table__body').innerText()
  h.check('操作显示中文名', body.includes('登记部署主机') && body.includes('越权请求被拒'), body.slice(0, 160))
  h.check('结果三态都能看到',
    body.includes('成功') && body.includes('拒绝') && body.includes('错误'), body.slice(0, 200))
  h.check('原始动作键保留', body.includes('deploy.host.add'))

  // UTC 03:04 在本地时区显示时不应还带 Z
  h.check('时间按本地渲染', !body.includes('T03:04:05Z'), body.slice(0, 120))

  // 按结果筛选
  await p.locator('.bar .el-radio-button', { hasText: '拒绝' }).click()
  await p.waitForTimeout(500)
  let rows = await p.locator('.el-table__body tbody tr').count()
  h.check('筛选拒绝只剩一条', rows === 1, rows)
  let text = await p.locator('.el-table__body').innerText()
  h.check('筛选后是拒绝那条', text.includes('越权请求被拒'), text.slice(0, 120))

  await p.locator('.bar .el-radio-button', { hasText: '全部' }).click()
  await p.waitForTimeout(400)
  rows = await p.locator('.el-table__body tbody tr').count()
  h.check('回到全部', rows === 3, rows)

  // 排序走 custom：点“操作者”表头不应报错，且行数不变
  await p.locator('.el-table__header th.is-sortable', { hasText: '操作者' }).click()
  await p.waitForTimeout(400)
  rows = await p.locator('.el-table__body tbody tr').count()
  h.check('排序后仍是全部行', rows === 3, rows)

  // 导出按钮在有数据时可用
  h.check('导出按钮可用',
    !(await p.locator('.panel-head button', { hasText: '导出 CSV' }).isDisabled()))

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
