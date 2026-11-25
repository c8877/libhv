// 监控 → 用量：报表页加载、指标切换、空状态、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/usage')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(800)

  h.check('监控组有用量菜单',
    (await p.locator('.el-menu-item', { hasText: '用量' }).count()) > 0)
  h.check('打开用量页', p.url().endsWith('/usage'), p.url())

  const head = await p.locator('.panel-head').first().innerText()
  h.check('标题与合计可见', head.includes('用量') && head.includes('合计'), head)

  const cols = await p.locator('.el-table__header th').allInnerTexts()
  h.check('表头包含日期/主体/指标/用量',
    ['日期', '主体', '指标', '用量', '事件数'].every((c) => cols.some((x) => x.includes(c))), cols)

  // 默认 7 天、空库时给出空状态
  h.check('默认显示空状态',
    (await p.locator('.el-table__empty-block', { hasText: '所选范围内没有用量' }).count()) === 1)

  // 租户筛选在指标之前，按顺序取第 2 个才是指标
  h.check('有租户筛选', (await p.locator('.panel-head .el-select').count()) >= 2)

  // 切换指标后仍能加载（会话时长）
  await p.locator('.panel-head .el-select').nth(1).click()
  await p.waitForSelector('.el-select-dropdown:visible', { timeout: 5000 })
  await p.locator('.el-select-dropdown__item', { hasText: '设备在线时长' }).click()
  await p.waitForTimeout(800)
  h.check('切换指标后无报错',
    (await p.locator('.panel-head').first().innerText()).includes('合计'))

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
