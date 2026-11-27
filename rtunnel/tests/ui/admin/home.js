// 首页：AI 助手对话页（参考 deepseek-chat），仪表盘挪到「概览」。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 900 } })
  await h.login(p)

  // 首页就是对话页：没有仪表盘卡片
  await p.goto(h.A + '/#/')
  await p.waitForSelector('.ai-chat', { timeout: 30000 })
  await p.waitForTimeout(800)
  h.check('首页是 AI 助手', await p.locator('.ai-chat').isVisible())
  h.check('首页不再有仪表盘卡片', (await p.locator('.cards').count()) === 0)
  h.check('首页不再是概览卡片页', (await p.locator('.el-descriptions').count()) === 0)

  const greeting = await p.locator('.empty-title').innerText()
  h.check('有对话页问候语', greeting.includes('rtunnel'), greeting)
  const chips = await p.locator('.ai-chat .chip').allInnerTexts()
  h.check('有快捷提问', chips.length >= 3, chips)
  h.check('消息区没有内部滚动容器', (await p.locator('.ai-chat .msgs .el-scrollbar').count()) === 0)

  const head = await p.locator('.ai-chat .panel-head').innerText()
  // 模型设置收在「更多」下拉里：头部只放历史/新对话，避免一行按钮像页签
  h.check('头部有历史/新对话', /历史/.test(head) && /新对话/.test(head), head.slice(0, 120))
  h.check('模型设置收进更多菜单', (await p.locator('.panel-head .more-btn').count()) === 1)
  await p.locator('.panel-head .more-btn').last().dispatchEvent('click')
  await p.waitForSelector('.el-dropdown-menu:visible', { timeout: 5000 })
  h.check('更多菜单里有模型设置',
    (await p.locator('.el-dropdown-menu:visible').innerText()).includes('模型设置'))
  await p.keyboard.press('Escape')
  await p.waitForTimeout(300)
  h.check('首页不再有全屏按钮', !/全屏/.test(head), head.slice(0, 120))
  h.check('有设备上下文选择', (await p.locator('.panel-head .el-select').count()) >= 1)

  // 未配置模型：点建议填进输入框并引导去配置，而不是发空请求
  await p.locator('.ai-chat .chip').first().click()
  const box = p.locator('.el-message-box, .el-message, .el-dialog')
  await box.first().waitFor({ timeout: 8000 }).catch(() => {})
  const balloon = await p.locator('.el-message, .el-message-box, .el-dialog').first().innerText().catch(() => '')
  h.check('未配置时引导去配置', /模型|服务商|Key/i.test(balloon), balloon.slice(0, 120))
  if (await p.locator('.el-dialog:visible').count()) {
    await p.locator('.el-dialog:visible').getByRole('button', { name: '取消' }).click({ force: true })
    await p.waitForTimeout(400)
  }
  await p.waitForSelector('.el-message', { state: 'detached', timeout: 8000 }).catch(() => {})
  h.check('没配置时输入内容还在', (await p.locator('.composer textarea').inputValue()).length > 0)
  await p.locator('.composer textarea').fill('')

  // 长对话时头部/输入框随页面滚，离开底部出现“回到底端”
  const cpos = await p.locator('.ai-chat .composer').evaluate((el) => getComputedStyle(el).position)
  const hpos = await p.locator('.ai-chat .panel-head').evaluate((el) => getComputedStyle(el).position)
  h.check('输入框随页面滚动', cpos !== 'sticky' && cpos !== 'fixed', cpos)
  h.check('头部随页面滚动', hpos !== 'sticky' && hpos !== 'fixed', hpos)
  await p.evaluate(() => {
    const msgs = document.querySelector('.ai-chat .msgs')
    const d = document.createElement('div')
    d.id = 'ui-tall'
    d.style.height = '1800px'
    msgs.appendChild(d)
    const pg = document.querySelector('.page')
    pg.scrollTop = 0
    pg.dispatchEvent(new Event('scroll'))
  })
  await p.waitForTimeout(400)
  h.check('往上翻出现回到底端', await p.locator('.to-bottom').isVisible())
  await p.locator('.to-bottom').dispatchEvent('click')
  await p.waitForTimeout(900)
  const nearEnd = await p.evaluate(() => {
    const pg = document.querySelector('.page')
    return pg.scrollHeight - pg.scrollTop - pg.clientHeight < 60
  })
  h.check('点回到底端直接到页面底端', nearEnd)
  await p.evaluate(() => { const d = document.getElementById('ui-tall'); if (d) d.remove() })

  // 仪表盘在「概览」，不再嵌助手
  await p.goto(h.A + '/#/overview')
  await p.waitForSelector('.cards', { timeout: 20000 })
  h.check('概览有统计卡片', (await p.locator('.cards .stat').count()) >= 4)
  h.check('概览不再嵌助手', (await p.locator('.ai-chat').count()) === 0)

  // 概览的 AI 诊断 → 回首页并带着问题
  await p.locator('button', { hasText: 'AI 诊断' }).first().click()
  await p.waitForTimeout(900)
  h.check('AI 诊断回到首页', !p.url().includes('#/overview') && (await p.locator('.ai-chat').count()) === 1,
    p.url())
  h.check('AI 诊断把问题带进输入框',
    (await p.locator('.composer textarea').inputValue()).includes('平台巡检'),
    await p.locator('.composer textarea').inputValue())
  if (await p.locator('.el-dialog:visible').count()) {
    await p.locator('.el-dialog:visible').getByRole('button', { name: '取消' }).click({ force: true })
    await p.waitForTimeout(400)
  }

  // 品牌区回首页：侧栏 logo
  await p.goto(h.A + '/#/usage')
  await p.waitForTimeout(500)
  await p.locator('aside.side .logo').dispatchEvent('click')
  await p.waitForTimeout(700)
  h.check('点侧栏品牌回对话首页', /#\/$/.test(p.url()) || p.url().endsWith('#/'), p.url())
  h.check('回到首页看到助手', await p.locator('.ai-chat').isVisible())

  // 手机上品牌在页头
  await p.setViewportSize({ width: 420, height: 820 })
  await p.waitForTimeout(400)
  h.check('手机页头有品牌', await p.locator('.topbar .brand-mini').isVisible())

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
