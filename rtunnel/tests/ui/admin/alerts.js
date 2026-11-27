// 系统 → 告警：列表、筛选、单条确认、全部确认、空状态
const h = require('../lib/harness')

const now = Date.now()

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  let state = 'open'
  let acked = []
  let ackAll = 0

  const ROWS = [
    {
      id: 3, at_ms: now - 60000, event: 'task.failed', level: 'critical',
      title: '分发任务失败', text: '任务 #12 → board01 failed：disk full',
      detail: { task_id: 12, devid: 'board01' }, acked_ms: 0, acked_by: '',
    },
    {
      id: 2, at_ms: now - 300000, event: 'device.offline', level: 'warning',
      title: '设备离线', text: '设备 board02 已离线超过 120 秒',
      detail: { devid: 'board02' }, acked_ms: 0, acked_by: '',
    },
    {
      id: 1, at_ms: now - 900000, event: 'cert.expiring', level: 'warning',
      title: '证书即将过期', text: '证书 a.example 剩余 20 天',
      detail: { san: 'a.example' }, acked_ms: now - 800000, acked_by: 'admin',
    },
  ]

  await p.route(/\/api\/v1\/alerts\?/, (route) => {
    const m = /state=([a-z]+)/.exec(route.request().url())
    state = m ? m[1] : 'open'
    let items = ROWS
    if (state === 'open') items = ROWS.filter((r) => !r.acked_ms)
    else if (state === 'acked') items = ROWS.filter((r) => r.acked_ms)
    route.fulfill({
      status: 200, contentType: 'application/json',
      body: JSON.stringify({
        items,
        counts: { total: ROWS.length, open: 2, critical: 1, warning: 2 },
      }),
    })
  })
  await p.route(/\/api\/v1\/alerts\/\d+\/ack$/, (route) => {
    acked.push(route.request().url())
    route.fulfill({ status: 200, contentType: 'application/json', body: '{"acked":true}' })
  })
  await p.route(/\/api\/v1\/alerts\/ack-all$/, (route) => {
    ackAll += 1
    route.fulfill({ status: 200, contentType: 'application/json', body: '{"acked":2}' })
  })

  await p.goto(h.A + '/#/alerts')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(900)

  h.check('系统组有告警菜单',
    (await p.locator('.el-menu-item', { hasText: '告警' }).count()) > 0)
  h.check('打开告警页', p.url().endsWith('/alerts'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('头部显示未确认与严重计数',
    head.includes('未确认 2') && head.includes('严重 1'), head)

  const rows0 = await p.locator('.el-table__body tbody tr').count()
  h.check('默认只列未确认', rows0 === 2, rows0)
  const body = await p.locator('.el-table__body').innerText()
  h.check('严重级别有标签', body.includes('严重') && body.includes('分发任务失败'), body.slice(0, 120))
  h.check('任务告警对象显示任务号', body.includes('任务 #12'), body.slice(0, 160))
  h.check('设备离线显示 devid', body.includes('devid:board02'))

  // 单条确认
  await p.locator('.el-table__body tbody tr').first()
    .locator('button', { hasText: '确认' }).click()
  await p.waitForTimeout(600)
  h.check('单条确认调用接口', acked.length === 1, acked)

  // 切到已确认
  await p.locator('.bar .el-radio-button', { hasText: '已确认' }).click()
  await p.waitForTimeout(600)
  h.check('按状态请求已确认', state === 'acked', state)
  const body2 = await p.locator('.el-table__body').innerText()
  h.check('已确认列表显示确认人', body2.includes('admin') && body2.includes('证书即将过期'),
    body2.slice(0, 120))

  // 全部确认（只在未确认页显示按钮）
  await p.locator('.bar .el-radio-button', { hasText: '未确认' }).click()
  await p.waitForTimeout(500)
  await p.locator('.panel-head button', { hasText: '全部确认' }).click()
  await p.waitForTimeout(600)
  h.check('全部确认调用接口', ackAll === 1, ackAll)

  // 空状态：没有未确认但有历史时给“看历史”
  await p.unroute(/\/api\/v1\/alerts\?/)
  await p.route(/\/api\/v1\/alerts\?/, (route) => route.fulfill({
    status: 200, contentType: 'application/json',
    body: JSON.stringify({ items: [], counts: { total: 3, open: 0, critical: 0, warning: 0 } }),
  }))
  await p.locator('.panel-head button', { hasText: '刷新' }).click()
  await p.waitForTimeout(700)
  const empty = await p.locator('.el-table__empty-block').innerText()
  h.check('空状态给出看历史', empty.includes('没有未确认的告警'), empty)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
