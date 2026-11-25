// 监控 → 费用：汇总表、价目表、调整单价
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/billing')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(800)

  h.check('监控组有费用菜单',
    (await p.locator('.el-menu-item', { hasText: '费用' }).count()) > 0)
  h.check('打开费用页', p.url().endsWith('/billing'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('标题与合计可见', head.includes('费用') && head.includes('合计'), head)

  const cols = await p.locator('.el-table').first().locator('.el-table__header th').allInnerTexts()
  h.check('汇总表头完整',
    ['租户', '指标', '用量', '计价单位', '单价', '金额'].every((c) => cols.some((x) => x.includes(c))),
    cols)

  // 价目表四条指标（用「币种」列定位，避免结算单/展开行改变下标）
  const priceTable = p.locator('.el-table').filter({
    has: p.locator('.el-table__header th', { hasText: '币种' }),
  })
  const priceRows = await priceTable.locator('.el-table__row').count()
  h.check('价目表有四个指标', priceRows === 4, 'rows=' + priceRows)

  // 调整会话次数单价
  const priceRow = priceTable.locator('.el-table__row', { hasText: '会话次数' })
  await priceRow.locator('button', { hasText: '调整' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  const priceInput = p.locator('.el-dialog:visible .el-input-number input').nth(1)
  await priceInput.fill('0.02')
  await p.locator('.el-dialog:visible .el-button--primary').click()
  await p.waitForTimeout(1200)
  const text = await priceTable.innerText()
  h.check('单价已更新', text.includes('0.02'), text.slice(0, 200))

  // 结算单面板：表头、生成、状态筛选
  const stmtPanel = p.locator('.panel').filter({
    has: p.locator('.panel-head h2', { hasText: '结算单' }),
  })
  const stmtHead = await stmtPanel.locator('.panel-head').innerText()
  h.check('结算单面板存在', stmtHead.includes('结算单'), stmtHead)
  const scols = await stmtPanel.locator('.el-table').first().locator('.el-table__header th').allInnerTexts()
  h.check('结算单表头完整',
    ['租户', '账期', '金额', '状态', '操作'].every((c) => scols.some((x) => x.includes(c))), scols)
  await stmtPanel.locator('button', { hasText: '生成 / 重算' }).click()
  const msg = p.locator('.el-message').filter({ hasText: /新建|重算|跳过/ }).last()
  await msg.waitFor({ timeout: 8000 })
  const msgText = await msg.innerText()
  h.check('生成结算单有反馈', /新建|重算|跳过/.test(msgText), msgText)
  // 状态筛选切到已出账，列表仍可正常渲染
  await stmtPanel.locator('.el-select').first().click()
  await p.locator('.el-select-dropdown__item', { hasText: '已出账' }).last().click()
  await p.waitForTimeout(600)
  h.check('状态筛选可用', (await stmtPanel.locator('.el-table').first().isVisible()), 'table hidden')

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
