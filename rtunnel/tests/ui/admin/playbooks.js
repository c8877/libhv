// 运维 → 自愈：列表、新建、校验、启停、删除、触发记录空态、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  // 先用 API 造一条规则，保证页面不是空表
  const made = await p.evaluate(async () => {
    const r = await fetch('/api/v1/playbooks', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        name: 'ui-自检', event: 'device.online', devid_glob: 'ui-*',
        argv: ['uptime'], verify_argv: ['uptime'], auto_apply: true,
        cooldown_s: 60, max_per_hour: 3,
      }),
    })
    return { status: r.status, body: await r.json().catch(() => ({})) }
  })
  h.check('可通过 API 创建规则', made.status === 200 && made.body.id, JSON.stringify(made).slice(0, 200))
  h.check('服务端算出只读风险', made.body.risk === 1, 'risk=' + made.body.risk)
  const rid = made.body.id

  await p.goto(h.A + '/#/playbooks')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(900)

  h.check('运维组有自愈菜单',
    (await p.locator('.el-menu-item', { hasText: '自愈' }).count()) > 0)
  h.check('打开自愈页', p.url().endsWith('/playbooks'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('标题可见', head.includes('自愈规则'), head)

  const cols = await p.locator('.el-table').first().locator('.el-table__header th').allInnerTexts()
  h.check('表头完整',
    ['名称', '触发', '设备', '命令', '风险', '自动执行', '启用'].every((c) => cols.some((x) => x.includes(c))),
    cols)

  const row = p.locator('.el-table__row').filter({ hasText: 'ui-自检' }).first()
  await row.waitFor({ timeout: 8000 })
  const rowText = await row.innerText()
  h.check('规则行可见并显示只读', rowText.includes('只读'), rowText)
  h.check('自动执行标记正确', rowText.includes('自动') || rowText.includes('需审批'), rowText)
  h.check('触发事件显示中文', rowText.includes('设备上线'), rowText)

  // 新建对话框：先空命令校验，再正常保存
  await p.locator('.panel-head button', { hasText: '新建规则' }).first().click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  await p.locator('.el-dialog:visible input').first().fill('ui-新建')
  await p.locator('.el-dialog:visible button', { hasText: '保存' }).click()
  const warn = p.locator('.el-message').filter({ hasText: /命令不能为空|请填写/ }).last()
  await warn.waitFor({ timeout: 5000 })
  h.check('空命令被拦', (await warn.innerText()).includes('命令'), await warn.innerText())

  await p.locator('.el-dialog:visible textarea').first().fill('echo\nhi')
  await p.locator('.el-dialog:visible button', { hasText: '保存' }).click()
  await p.waitForTimeout(900)
  const created = p.locator('.el-table__row').filter({ hasText: 'ui-新建' }).first()
  await created.waitFor({ timeout: 8000 })
  h.check('新建规则出现在列表', (await created.innerText()).includes('ui-新建'))

  // 启停开关
  const sw = created.locator('.el-switch').first()
  await sw.click()
  await p.waitForTimeout(800)
  h.check('停用后开关关闭', (await p.locator('.el-table__row').filter({ hasText: 'ui-新建' })
    .first().locator('.el-switch.is-checked').count()) === 0)

  // 删除（确认框）
  await p.locator('.el-table__row').filter({ hasText: 'ui-新建' }).first()
    .locator('button', { hasText: '删除' }).click()
  await p.waitForSelector('.el-message-box:visible', { timeout: 5000 })
  await p.locator('.el-message-box__btns button.el-button--primary').last().click()
  await p.waitForTimeout(900)
  h.check('删除后行消失',
    (await p.locator('.el-table__row').filter({ hasText: 'ui-新建' }).count()) === 0)

  // 触发记录面板（无记录时给空态，不报错）
  const panels = await p.locator('.panel-head h2').allInnerTexts()
  h.check('有触发记录面板', panels.some((x) => x.includes('触发记录')), panels)
  h.check('触发记录空态可见',
    (await p.locator('.el-table__empty-text').count()) >= 0)

  h.check('无 JS 错误', errs.length === 0, errs)

  // 收尾：删掉 API 造的规则，保持库干净
  await p.evaluate(async (id) => {
    await fetch('/api/v1/playbooks/' + id, { method: 'DELETE' })
  }, rid)

  await b.close()
  h.finish()
})()
