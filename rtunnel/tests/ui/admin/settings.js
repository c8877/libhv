// 系统 → 设置：页内锚点只滚容器不改路由、分组编辑、只提交改动项
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 900 } })
  await h.login(p)

  const puts = []
  await p.route(/\/api\/v1\/settings$/, async (route) => {
    if (route.request().method() === 'PUT') {
      puts.push(JSON.parse(route.request().postData() || '{}'))
    }
    return route.continue()
  })

  await p.goto(h.A + '/#/settings')
  await p.waitForSelector('.set-wrap', { timeout: 20000 })
  await p.waitForTimeout(900)

  // hash 路由下，锚点 href="#set-3" 不能被当成路由跳走
  const before = p.url()
  const links = await p.locator('.set-nav a').count()
  h.check('有页内分组导航', links > 3, links)
  await p.locator('.set-nav a').nth(3).click()
  await p.waitForTimeout(900)
  h.check('点锚点后路由不变', p.url() === before, before + ' -> ' + p.url())
  h.check('点锚点后滚动容器动了',
    (await p.locator('.page').evaluate((el) => el.scrollTop)) > 100,
    await p.locator('.page').evaluate((el) => el.scrollTop))
  h.check('当前分组高亮',
    (await p.locator('.set-nav .el-anchor__link.is-active').count()) === 1)

  // 滚一段后左侧导航仍贴在上方（sticky），不会跟着滚没
  const navTop = await p.locator('.set-nav').evaluate((el) => el.getBoundingClientRect().top)
  h.check('分组导航吸顶', navTop >= 0 && navTop < 200, navTop)

  // 分组编辑：第一组只改一个值，PUT 里不应该带整张表
  const firstPanel = p.locator('.set-body .panel').first()
  await firstPanel.locator('.panel-head button', { hasText: '编辑' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  await p.waitForTimeout(500)
  const nums = p.locator('.el-dialog:visible .el-input-number input')
  h.check('数字项用 el-input-number', (await nums.count()) >= 1, await nums.count())
  const numCount = await nums.count()
  if (numCount > 0) {
    await nums.first().fill('3')
    await p.locator('.el-dialog:visible button', { hasText: '保存' }).click()
    await p.waitForTimeout(900)
    h.check('保存只提交改动项', puts.length === 1 && Object.keys(puts[0]).length === 1,
      JSON.stringify(puts))
    h.check('保存后弹窗关闭', (await p.locator('.el-dialog:visible').count()) === 0)
  }

  // 告警组带测试与跳转入口
  const alertBar = p.locator('.alert-bar')
  h.check('告警组有测试按钮', (await alertBar.locator('button', { hasText: '发送测试' }).count()) === 1)
  h.check('告警组能跳到告警页',
    (await alertBar.locator('button', { hasText: '看告警列表' }).count()) === 1)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
