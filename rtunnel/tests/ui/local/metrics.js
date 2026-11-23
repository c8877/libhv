// 设备本地页的监控指标：默认选中进程 chip、可隐藏、采样配置只读。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1300, height: 950 } })
  const puts = []
  p.on('request', (r) => {
    if (r.method() === 'PUT' && /metrics\/series\/config/.test(r.url())) puts.push(r.url())
  })

  await p.goto(h.L + '/')
  await p.waitForSelector('.dev-tabs', { timeout: 30000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForTimeout(3000)

  let chips = await p.locator('.series-select .el-tag').allInnerTexts()
  // 进程名要等采样点到达才显示，没到时是「PID 1 CPU」
  const isProc1 = (t) => /(\[1\]|PID 1)/.test(t)
  h.check('本地页默认显示进程 chip', chips.some(isProc1), chips)

  const procTag = p.locator('.series-select .el-tag', { hasText: /(\[1\]|PID 1)/ })
    .filter({ hasText: 'CPU' }).first()
  const close = procTag.locator('.el-tag__close')
  h.check('进程 chip 有关闭按钮', (await close.count()) === 1)
  await close.click()
  await p.waitForTimeout(900)
  chips = await p.locator('.series-select .el-tag').allInnerTexts()
  h.check('可以移除进程 chip', !chips.some((t) => isProc1(t) && /CPU/.test(t)), chips)
  h.check('本地页不移除采样配置', puts.length === 0, puts)

  const tip = await p.locator('.series-tip').innerText().catch(() => '')
  h.check('本地页提示只读', tip.includes('本地页只读'), tip)

  await p.locator('.monitor-switch .el-segmented__item', { hasText: '采样配置' }).click()
  await p.waitForTimeout(1200)
  const bar = await p.locator('.feat-panel .tab-bar').last().innerText()
  h.check('本地页标注采样只读', bar.includes('本地页只读'), bar)
  const disabled = await p.locator('.el-table__body .el-switch')
    .evaluateAll((els) => els.length > 0 && els.every((e) => e.classList.contains('is-disabled')))
  h.check('本地页开关为只读', disabled)
  h.check('本地页无新增指标按钮',
    (await p.locator('.feat-panel .el-button', { hasText: '添加指标' }).count()) === 0)

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 4))
  await b.close()
  h.finish()
})()
