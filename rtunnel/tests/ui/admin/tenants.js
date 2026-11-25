// 监控 → 租户：默认租户可见、新建租户、编辑与状态
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/tenants')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(800)

  h.check('监控组有租户菜单',
    (await p.locator('.el-menu-item', { hasText: '租户' }).count()) > 0)
  h.check('打开租户页', p.url().endsWith('/tenants'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('标题可见', head.includes('租户'), head)

  const cols = await p.locator('.el-table__header th').allInnerTexts()
  h.check('表头包含租户/状态/账号/设备/套餐',
    ['租户', '状态', '账号', '设备', '套餐'].every((c) => cols.some((x) => x.includes(c))), cols)

  h.check('默认租户在列表里',
    (await p.locator('.el-table__row', { hasText: '默认租户' }).count()) >= 1)

  // 新建
  const name = 'ui-' + Date.now()
  await p.locator('.panel-head .el-button', { hasText: '新建租户' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  await p.locator('.el-dialog:visible input').first().fill(name)
  await p.locator('.el-dialog:visible .el-button--primary').click()
  await p.waitForTimeout(1200)
  h.check('新建后出现在列表',
    (await p.locator('.el-table__row', { hasText: name }).count()) === 1, name)

  // 编辑：停用后状态列变灰
  const row = p.locator('.el-table__row', { hasText: name })
  await row.locator('button', { hasText: '编辑' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  // 套餐：并发 2、月流量 1.5 GiB
  const nums = p.locator('.el-dialog:visible .el-input-number input')
  await nums.nth(1).fill('2')
  await nums.nth(2).fill('1.5')
  await p.locator('.el-dialog:visible .el-switch').click()
  await p.locator('.el-dialog:visible .el-button--primary').click()
  await p.waitForTimeout(400)
  // 停用需要二次确认
  const confirm = p.locator('.el-message-box__btns .el-button--primary')
  if (await confirm.count()) {
    await confirm.click()
    await p.waitForTimeout(1200)
  }
  const rowText = await p.locator('.el-table__row', { hasText: name }).innerText()
  h.check('停用后状态显示停用', rowText.includes('停用'), rowText)
  h.check('套餐已保存并展示', rowText.includes('并发 2') && rowText.includes('1.5 GiB'),
    rowText.slice(0, 200))

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
