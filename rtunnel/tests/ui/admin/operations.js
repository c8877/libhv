// 运维 → 作业：列表、状态切换、待审批动作、详情弹层、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  // 造一个待审批作业（risk 2：/bin/sh -c 会被判为改状态）
  const made = await p.evaluate(async () => {
    const r = await fetch('/api/v1/operations/plan', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ kind: 'exec', devid: 'ui-dev', argv: ['/bin/sh', '-c', 'echo ui'] }),
    })
    return { status: r.status, body: await r.json().catch(() => ({})) }
  })
  h.check('可通过 API 创建待审批作业', made.status === 200 && made.body.status === 'pending_approval',
    JSON.stringify(made))

  await p.goto(h.A + '/#/operations')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(800)

  h.check('运维组有作业菜单',
    (await p.locator('.el-menu-item', { hasText: '作业' }).count()) > 0)
  h.check('打开作业页', p.url().endsWith('/operations'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('标题可见', head.includes('操作作业'), head)

  const cols = await p.locator('.el-table__header th').allInnerTexts()
  h.check('表头完整',
    ['设备', '命令', '风险', '状态', '发起人', '审批人'].every((c) => cols.some((x) => x.includes(c))),
    cols)

  const tabs = await p.locator('.el-tabs__item').allInnerTexts()
  h.check('状态页签完整',
    ['待审批', '待执行', '已执行', '失败', '全部'].every((c) => tabs.some((x) => x.includes(c))), tabs)

  // 默认就在「待审批」，刚造的作业应出现并带批准/拒绝按钮
  const row = p.locator('.el-table__row').filter({ hasText: 'ui-dev' }).first()
  await row.waitFor({ timeout: 8000 })
  const rowText = await row.innerText()
  h.check('待审批作业可见', rowText.includes('待审批'), rowText)
  h.check('风险显示改状态', rowText.includes('改状态'), rowText)
  h.check('有批准与拒绝按钮',
    (await row.locator('button', { hasText: '批准' }).count()) === 1 &&
    (await row.locator('button', { hasText: '拒绝' }).count()) === 1)

  // 自己批自己：后端拒绝，前端应弹错误
  await row.locator('button', { hasText: '批准' }).click()
  await p.waitForSelector('.el-message-box:visible', { timeout: 5000 })
  await p.locator('.el-message-box:visible .el-message-box__input input').fill('ui test')
  await p.locator('.el-message-box__btns button.el-button--primary').last().click()
  const msg = p.locator('.el-message').filter({ hasText: /不能批准自己的|失败/ }).last()
  await msg.waitFor({ timeout: 8000 })
  h.check('自批被拒并提示', (await msg.innerText()).includes('不能批准自己'), await msg.innerText())

  // 详情弹层
  await row.locator('button', { hasText: '详情' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  const dlg = await p.locator('.el-dialog:visible').innerText()
  h.check('详情含 plan_hash 与计划',
    dlg.includes('plan_hash') && dlg.includes('argv'), dlg.slice(0, 200))
  await p.locator('.el-dialog:visible button', { hasText: '关闭' }).click()

  // 切到「全部」仍可渲染
  await p.locator('.el-tabs__item', { hasText: '全部' }).click()
  await p.waitForTimeout(600)
  h.check('切到全部仍可渲染', (await p.locator('.el-table').isVisible()))

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
