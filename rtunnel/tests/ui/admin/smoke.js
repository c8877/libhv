// 管理台设备详情页总览：主 tab、KPI、吸顶、各 tab 渲染、电源确认、移动端。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { ctx, page: p, errs } = await h.openPage(b)
  let powerPosts = 0
  p.on('request', (r) => {
    if (r.method() === 'POST' && /\/power$/.test(r.url())) powerPosts++
  })

  await h.login(p)
  await h.openDevice(p)
  await p.waitForSelector('.ov-wrap', { timeout: 30000 })
  await p.waitForTimeout(8000)

  const tabs = await p.locator('.dev-tabs .el-tabs__item').allInnerTexts()
  h.check('九个主 tab', tabs.length === 9 && tabs[0] === '概览' && tabs[8] === '能力', tabs)

  const kpis = await p.locator('.ov-kpi').count()
  h.check('四个 KPI', kpis === 4, kpis)
  const kpiText = await p.locator('.ov-kpis').innerText()
  h.check('KPI 有数值',
    /CPU/.test(kpiText) && /%/.test(kpiText) && /负载/.test(kpiText) && /累计流量/.test(kpiText),
    kpiText.replace(/\n/g, ' ').slice(0, 160))

  const ovText = await p.locator('.ov-wrap').innerText()
  h.check('资源趋势卡', ovText.includes('资源趋势'), ovText.slice(0, 60))
  h.check('存储/网卡/系统卡',
    ovText.includes('存储') && ovText.includes('网卡') && ovText.includes('系统信息'), ovText.slice(0, 160))
  h.check('趋势图存在', (await p.locator('.ov-spark svg').count()) === 1)

  // 粘性头：概览可能不足一屏，切到进程再滚动
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '进程' }).click()
  await p.waitForTimeout(1200)
  await p.setViewportSize({ width: 1440, height: 700 })
  await p.evaluate(() => { const el = document.querySelector('.page'); if (el) el.scrollTop = 700 })
  await p.waitForTimeout(500)
  const sticky = await p.evaluate(() => {
    const pg = document.querySelector('.page')
    return {
      pageScroll: Math.round(pg.scrollTop),
      pageTop: Math.round(pg.getBoundingClientRect().top),
      stickyTop: Math.round(document.querySelector('.dev-sticky').getBoundingClientRect().top),
    }
  })
  h.check('摘要吸顶', sticky.pageScroll > 0 && Math.abs(sticky.stickyTop - sticky.pageTop) <= 2, sticky)
  await p.evaluate(() => { const el = document.querySelector('.page'); if (el) el.scrollTop = 0 })
  await p.waitForTimeout(300)
  await p.setViewportSize({ width: 1440, height: 1000 })
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '概览' }).click()
  await p.waitForTimeout(500)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '网络' }).click()
  await p.waitForTimeout(1500)
  h.check('网络面板可见', await p.locator('.net-panel').isVisible())
  const netTabs = await p.locator('.net-tabs .el-tabs__item').allInnerTexts()
  h.check('网络五个子 tab', netTabs.length === 5 && netTabs[0].includes('网卡'), netTabs)
  h.check('网卡有数据', (await p.locator('.net-table .el-table__body tbody tr').count()) >= 1)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '存储' }).click()
  await p.waitForTimeout(1200)
  h.check('存储 tab 有挂载点', (await p.locator('.feat-panel .store-head').count()) >= 1)
  h.check('概览已隐藏', (await p.locator('.ov-wrap').count()) === 0)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '文件' }).click()
  await p.waitForTimeout(2000)
  h.check('文件管理器', (await p.locator('.crumb').count()) > 0)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '服务' }).click()
  await p.waitForTimeout(3000)
  h.check('服务管理器', (await p.locator('.feat-panel table').count()) > 0)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '进程' }).click()
  await p.waitForTimeout(1500)
  h.check('进程摘要', /进程/.test(await p.locator('.tab-bar').first().innerText()))

  // 能力：默认折叠，展开后可见电源开关
  await p.locator('.dev-tabs .el-tabs__item', { hasText: '能力' }).click()
  await p.waitForTimeout(900)
  h.check('能力表单默认折叠', (await p.locator('.caps-grid').count()) === 0)
  const editBtn = p.locator('.caps-runtime button', { hasText: '编辑配置' })
  h.check('有编辑配置按钮', (await editBtn.count()) === 1)
  await editBtn.click()
  await p.waitForTimeout(500)
  h.check('展开后显示表单', (await p.locator('.caps-grid').count()) === 1)
  const capsText = await p.locator('.caps-grid').innerText()
  h.check('重启/关机在能力配置里',
    capsText.includes('允许重启') && capsText.includes('允许关机'), capsText.replace(/\n/g, ' ').slice(0, 160))

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '监控' }).click()
  await p.waitForTimeout(1500)
  h.check('监控曲线', (await p.locator('.chart-box canvas').count()) > 0)
  const sub = await p.locator('.monitor-switch .el-segmented__item').allInnerTexts()
  h.check('监控子切换', sub.length === 2 && sub[0] === '曲线', sub)
  await p.locator('.monitor-switch .el-segmented__item', { hasText: '采样配置' }).click()
  await p.waitForTimeout(700)
  h.check('采样配置表', (await p.locator('.feat-panel .el-table').count()) > 0)

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '串口' }).click()
  await p.waitForTimeout(900)
  const ttyText = await p.locator('.obs-panel').innerText()
  h.check('串口页', ttyText.includes('已配置') || ttyText.includes('暂无'), ttyText.slice(0, 80))

  await p.locator('.dev-tabs .el-tabs__item', { hasText: '概览' }).click()
  await p.waitForTimeout(600)
  h.check('回到概览', await p.locator('.ov-wrap').isVisible())

  // 电源确认框必须输入 root 密码
  await p.locator('.dh-top button', { hasText: '更多' }).click()
  await p.waitForTimeout(300)
  const rb = p.locator('.el-dropdown-menu__item', { hasText: '重启设备' })
  h.check('重启菜单可用', (await rb.getAttribute('disabled')) === null)
  await rb.click()
  await p.waitForTimeout(700)
  const prompt = await p.evaluate(() => {
    const box = document.querySelector('.el-message-box')
    const input = box && box.querySelector('input')
    return { visible: !!box && box.getBoundingClientRect().width > 200, type: input ? input.type : '' }
  })
  h.check('电源确认框为密码输入', prompt.visible && prompt.type === 'password', prompt)
  await p.locator('.el-message-box__btns button', { hasText: '取消' }).click()
  await p.waitForTimeout(500)
  h.check('取消后不发送电源请求', powerPosts === 0, powerPosts)

  // 手机
  const state = await ctx.storageState()
  const { ctx: mctx, page: mp, errs: merrs } = await h.openPage(b, {
    storageState: state,
    viewport: { width: 390, height: 844 },
  })
  await mp.goto(h.A + '/#/agents/' + h.DEV)
  await mp.waitForSelector('.ov-wrap', { timeout: 30000 })
  await mp.waitForTimeout(1500)
  const mOver = await mp.evaluate(() => ({
    s: document.documentElement.scrollWidth,
    c: document.documentElement.clientWidth,
  }))
  h.check('手机无横向溢出', mOver.s <= mOver.c + 1, mOver)
  h.check('手机四个 KPI', (await mp.locator('.ov-kpi').count()) === 4)
  const mHeadTop = await mp.evaluate(() => Math.round(document.querySelector('.dev-head').getBoundingClientRect().top))
  h.check('手机摘要可见', mHeadTop >= 0, mHeadTop)
  await mp.locator('.dev-tabs .el-tabs__item', { hasText: '网络' }).click()
  await mp.waitForTimeout(1500)
  h.check('手机网络面板', (await mp.locator('.net-panel').count()) === 1)
  h.check('手机无 JS 错误', merrs.length === 0, merrs.slice(0, 3))
  await h.shot(mp, 'smoke-mobile')
  await mctx.close()

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await b.close()
  h.finish()
})()
