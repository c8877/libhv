// 监控指标：进程级 CPU/内存采样
//   - 指标下拉 footer 的「添加进程…」按名字搜索、勾选 CPU/内存
//   - 添加后配置下发、chip 显示、曲线出点
//   - 刷新 / 切走再回来，chip 选择仍保留
//   - 取消 chip 停止采样、清空数据，采样配置保留停用项并可重新启用
//   - 两台设备之间不串指标
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1360, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/agents/' + h.DEV)
  await p.waitForSelector('.dev-tabs', { timeout: 30000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForSelector('.series-select', { timeout: 25000 })
  await p.waitForTimeout(800)

  // ---- 添加进程指标 ----
  await p.locator('.series-select .el-select__wrapper').click()
  await p.waitForTimeout(400)
  const dd = p.locator('.el-select-dropdown').filter({ has: p.locator('.series-pop-actions') })
  h.check('指标下拉已打开', await dd.isVisible().catch(() => false))
  await dd.getByRole('button', { name: '添加进程…' }).click()
  const pick = p.locator('.el-dialog', { hasText: '添加进程指标' })
  await p.waitForSelector('.el-dialog:has-text("添加进程指标") .el-table__body tbody tr', { timeout: 30000 })
  await p.waitForTimeout(400)

  // 搜一个稳定进程：测试用的 agent 自己
  const query = pick.locator('.proc-pick-query input')
  await query.fill('rtunnel-agent')
  await query.press('Enter')
  await p.waitForTimeout(1200)
  const rows = pick.locator('.el-table__body tbody tr')
  h.check('按名字搜索到进程', (await rows.count()) >= 1, await rows.count())
  const first = rows.first()
  const pid = (await first.locator('td').first().innerText()).trim()
  const comm = (await first.locator('td').nth(1).locator('b').innerText()).trim()
  h.check('搜到的是 agent 进程', comm === 'rtunnel-agent', comm)

  await first.locator('.el-checkbox', { hasText: 'CPU' }).click()
  await p.waitForTimeout(150)
  await first.locator('.el-checkbox', { hasText: '内存' }).click()
  await p.waitForTimeout(200)
  const addBtn = pick.locator('.el-dialog__footer').getByRole('button', { name: /添加/ })
  h.check('添加按钮显示 2 项', /添加 2 项/.test(await addBtn.innerText()), await addBtn.innerText())
  const [addResp] = await Promise.all([
    p.waitForResponse((r) => /\/metrics\/series\/config$/.test(r.url()) && r.request().method() === 'PUT', { timeout: 25000 }),
    addBtn.click(),
  ])
  h.check('添加进程指标后配置已下发', addResp.ok(), addResp.status())
  await p.waitForTimeout(4500)

  const cpuSeries = 'proc:' + pid + ':cpu'
  const memSeries = 'proc:' + pid + ':mem'
  const cfg = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV + '/metrics/series/config')).json()
  const cfgCpu = (cfg || []).find((x) => x.series === cpuSeries)
  h.check('配置包含进程 CPU 指标', !!cfgCpu && cfgCpu.enabled === true, cfgCpu)
  h.check('配置包含进程内存指标', !!(cfg || []).find((x) => x.series === memSeries && x.enabled))

  const chipText = await p.locator('.series-select').innerText()
  h.check('chip 显示进程名和 PID', chipText.includes('[' + pid + ']'), chipText.slice(0, 200))

  const data = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV
    + '/metrics/series?series=' + encodeURIComponent(cpuSeries) + '&since_ts=0')).json()
  const pts = data.points || []
  h.check('进程 CPU 曲线有采样点', pts.length > 0, pts.length)
  h.check('采样点带进程名', pts.length > 0 && pts[pts.length - 1].name === comm, pts.slice(-1))
  h.check('采样点数值有效', pts.length > 0 && Number.isFinite(Number(pts[pts.length - 1].v)), pts.slice(-1))
  h.check('图表已绘制', (await p.locator('.chart-box canvas').count()) > 0)

  // 采样配置里显示友好名称
  await p.locator('.monitor-switch .el-segmented__item', { hasText: '采样配置' }).click()
  await p.waitForTimeout(900)
  const tableText = await p.locator('.obs-panel .el-table').innerText()
  h.check('采样配置显示进程标签', tableText.includes('[' + pid + ']'), tableText.slice(0, 400))
  await p.locator('.monitor-switch .el-segmented__item', { hasText: '曲线' }).click()
  await p.waitForTimeout(600)

  // ---- 刷新 / 切走再回来，选择仍保留 ----
  await p.reload()
  await p.waitForSelector('.dev-tabs', { timeout: 30000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForTimeout(3500)
  let chips = await p.locator('.series-select .el-tag').allInnerTexts()
  h.check('刷新后仍选中进程', chips.some((t) => t.includes('[' + pid + ']')), chips)

  await p.goto(h.A + '/#/agents')
  await p.waitForTimeout(900)
  await p.goto(h.A + '/#/agents/' + h.DEV)
  await p.waitForSelector('.dev-tabs', { timeout: 30000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForTimeout(3000)
  chips = await p.locator('.series-select .el-tag').allInnerTexts()
  h.check('返回详情后仍选中进程', chips.some((t) => t.includes('[' + pid + ']')), chips)

  // ---- 取消 chip 停止采样 ----
  const chip = p.locator('.series-select .el-tag', { hasText: '[' + pid + '] CPU' }).first()
  h.check('进程 CPU chip 存在', (await chip.count()) > 0)
  const [offResp] = await Promise.all([
    p.waitForResponse((r) => /\/metrics\/series\/config$/.test(r.url()) && r.request().method() === 'PUT', { timeout: 25000 }),
    chip.locator('.el-tag__close').click(),
  ])
  const offBody = offResp.request().postDataJSON()
  const offRow = (offBody || []).find((x) => x.series === cpuSeries)
  h.check('取消 chip 下发 enabled=false', !!offRow && offRow.enabled === false, offRow)
  await p.waitForTimeout(1500)

  const cfgAfter = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV + '/metrics/series/config')).json()
  const rowAfter = (cfgAfter || []).find((x) => x.series === cpuSeries)
  h.check('配置里该进程指标已停用', !!rowAfter && rowAfter.enabled === false, rowAfter)
  const after = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV
    + '/metrics/series?series=' + encodeURIComponent(cpuSeries) + '&since_ts=0')).json()
  h.check('取消后曲线数据已清空', (after.points || []).length === 0, (after.points || []).length)
  h.check('chip 已从选择器移除',
    (await p.locator('.series-select .el-tag', { hasText: '[' + pid + '] CPU' }).count()) === 0)
  // 内存指标仍在采
  const memCfg = (cfgAfter || []).find((x) => x.series === memSeries)
  h.check('只停了被取消的那一项', !!memCfg && memCfg.enabled === true, memCfg)

  // ---- 采样配置里重新启用 ----
  await p.locator('.monitor-switch .el-segmented__item', { hasText: '采样配置' }).click()
  await p.waitForTimeout(900)
  // 停用后曲线数据已清空，标签可能退回「PID <pid> CPU」，两种写法都认
  const procLabel = new RegExp('(\\[' + pid + '\\]|PID ' + pid + ')')
  const cfgRow = p.locator('.row-edit-table .el-table__body tbody tr', { hasText: procLabel })
    .filter({ hasText: 'CPU' }).first()
  h.check('采样配置里保留停用项', (await cfgRow.count()) > 0)
  if ((await cfgRow.count()) > 0) {
    const [onResp] = await Promise.all([
      p.waitForResponse((r) => /\/metrics\/series\/config$/.test(r.url()) && r.request().method() === 'PUT', { timeout: 25000 }),
      cfgRow.locator('.el-switch').first().click(),
    ])
    const onRow = (onResp.request().postDataJSON() || []).find((x) => x.series === cpuSeries)
    h.check('采样配置可重新启用', !!onRow && onRow.enabled === true, onRow)
    await p.waitForTimeout(1800)
    const cfg2 = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV + '/metrics/series/config')).json()
    h.check('恢复后配置为启用',
      !!(cfg2 || []).find((x) => x.series === cpuSeries && x.enabled === true))
  }

  // ---- 两台设备之间不串指标 ----
  await p.evaluate((dev) => { location.hash = '#/agents/' + dev }, h.DEV2)
  await p.waitForTimeout(2000)
  await p.waitForSelector('.dev-tabs', { timeout: 30000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForTimeout(3500)
  chips = await p.locator('.series-select .el-tag').allInnerTexts()
  h.check('B 不显示 A 的进程 chip', !chips.some((t) => t.includes('[' + pid + ']')), chips)

  await p.locator('.series-select .el-select__wrapper').click()
  await p.waitForTimeout(600)
  const opts = await p.locator('.el-select-dropdown__item').allInnerTexts()
  h.check('B 的下拉不含 A 的进程', !opts.some((t) => t.includes('[' + pid + ']')), opts)
  const cfgB = await (await p.request.get(h.A + '/api/v1/agents/' + h.DEV2 + '/metrics/series/config')).json()
  h.check('B 的配置只回 B 的指标', Array.isArray(cfgB) && !(cfgB || []).some((x) => String(x.series).startsWith('proc:')),
    cfgB)

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await h.shot(p, 'metrics-device-b')
  await b.close()
  h.finish()
})()
