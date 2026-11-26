// 首页仪表盘：内嵌 AI 助手、快捷提问、模型设置入口、点品牌回首页。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)

  // 首页就是概览：助手面板在统计卡片下方
  await p.goto(h.A + '/#/')
  await p.waitForSelector('.ai-chat', { timeout: 30000 })
  await p.waitForTimeout(800)

  h.check('首页有 AI 助手面板', await p.locator('.ai-chat').isVisible())
  h.check('仪表盘统计仍在', (await p.locator('.cards .stat').count()) >= 4)
  const head = await p.locator('.ai-chat .panel-head').innerText()
  h.check('助手在首页不是整页', head.includes('AI 助手') && !(await p.locator('.chat-page').count()),
    head.slice(0, 80))

  // 快捷提问：未配置模型时应提示去配置，而不是发空请求
  const chips = await p.locator('.ai-chat .chip').allInnerTexts()
  h.check('首页有快捷提问', chips.length >= 3, chips)
  await p.locator('.ai-chat .chip').first().click()
  const box = p.locator('.el-message-box, .el-message, .el-dialog')
  await box.first().waitFor({ timeout: 8000 }).catch(() => {})
  const balloon = await p.locator('.el-message, .el-message-box, .el-dialog').first().innerText().catch(() => '')
  h.check('未配置时引导去配置', /模型|服务商|Key/i.test(balloon), balloon.slice(0, 120))
  // 如果是设置弹窗，关掉它，避免影响后续
  if (await p.locator('.el-dialog:visible').count()) {
    await p.locator('.el-dialog:visible').getByRole('button', { name: '取消' }).click({ force: true })
    await p.waitForTimeout(400)
  }

  // 等提示自己消失，否则会挡住后面的点击
  await p.waitForSelector('.el-message', { state: 'detached', timeout: 8000 }).catch(() => {})

  // 消息区不是内部小滚动条：气泡直接追加在页面里
  h.check('消息区没有内部滚动容器', (await p.locator('.ai-chat .msgs .el-scrollbar').count()) === 0)

  // 全屏入口：原地展开，不跳路由、不丢会话
  const full = await p.locator('.ai-chat .panel-head button').filter({ hasText: '全屏' }).count()
  h.check('首页助手有全屏入口', full === 1)

  const ta = p.locator('.ai-chat textarea')
  await ta.fill('未发送的草稿')
  await p.locator('.ai-chat .panel-head button').filter({ hasText: '全屏' }).first()
    .dispatchEvent('click')
  await p.waitForTimeout(800)
  h.check('全屏后仍在首页', !p.url().includes('#/assistant'), p.url())
  h.check('全屏后占满主区', await p.locator('.ai-chat.full').isVisible())
  h.check('全屏后统计卡片收起', !(await p.locator('.cards').isVisible()))
  h.check('展开后草稿还在（同一个组件实例）',
    (await p.locator('.ai-chat textarea').inputValue()) === '未发送的草稿')
  const cpos = await p.locator('.ai-chat.full .composer')
    .evaluate((el) => getComputedStyle(el).position)
  const hpos = await p.locator('.ai-chat.full .panel-head')
    .evaluate((el) => getComputedStyle(el).position)
  h.check('输入框随页面滚动', cpos !== 'sticky' && cpos !== 'fixed', cpos)
  h.check('头部随页面滚动', hpos !== 'sticky' && hpos !== 'fixed', hpos)

  // 造一段长内容：滚上去后出现“回到底端”，点了直接到页面底端
  await p.evaluate(() => {
    const msgs = document.querySelector('.ai-chat .msgs')
    if (!msgs) return
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

  // 退出全屏：回到仪表盘，草稿和会话都还在
  await p.locator('.ai-chat .panel-head button').filter({ hasText: '退出全屏' }).first()
    .dispatchEvent('click')
  await p.waitForTimeout(700)
  h.check('退出全屏后卡片回来', await p.locator('.cards').isVisible())
  h.check('退出全屏后仍是首页', !p.url().includes('#/assistant'), p.url())
  h.check('退出全屏后草稿还在',
    (await p.locator('.ai-chat textarea').inputValue()) === '未发送的草稿')
  await p.locator('.ai-chat textarea').fill('')

  // 品牌区回首页：侧栏 logo
  await p.locator('aside.side .logo').dispatchEvent('click')
  await p.waitForTimeout(600)
  h.check('点侧栏品牌回首页', /#\/(overview)?$/.test(p.url()), p.url())

  // 手机上品牌在页头
  await p.setViewportSize({ width: 420, height: 820 })
  await p.waitForTimeout(400)
  h.check('手机页头有品牌', await p.locator('.topbar .brand-mini').isVisible())
  await p.goto(h.A + '/#/usage')
  await p.waitForTimeout(600)
  await p.locator('.topbar .brand-mini').dispatchEvent('click')
  await p.waitForTimeout(600)
  h.check('手机点品牌回首页', !p.url().includes('#/usage'), p.url())

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
