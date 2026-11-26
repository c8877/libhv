// AI 助手历史会话：抽屉、切换、改名、删除、翻页、刷新后仍在
//
// 会话记录不依赖模型：这里用接口直接造空会话，验证的是“历史”这一层的交互。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  // 用页面里的 fetch（带 Cookie）造两条会话，避免依赖模型配置
  const made = await p.evaluate(async () => {
    const mk = async (title) => {
      const r = await fetch('/api/v1/ai/sessions', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ title }),
      })
      return r.ok ? (await r.json()).id : 0
    }
    const a = await mk('UI 会话一')
    const bb = await mk('UI 会话二')
    return [a, bb]
  })
  h.check('能创建测试会话', made[0] > 0 && made[1] > 0, JSON.stringify(made))

  await p.goto(h.A + '/#/assistant')
  await p.waitForSelector('.ai-chat', { timeout: 20000 })
  await p.waitForTimeout(800)

  // 1) 顶部有历史入口，且带数量
  const histBtn = p.locator('.panel-head button').filter({ hasText: '历史' }).first()
  h.check('有历史入口', (await histBtn.count()) === 1)
  h.check('历史入口带数量', /（\d+）/.test(await histBtn.innerText()), await histBtn.innerText())

  await histBtn.click()
  await p.waitForTimeout(1200)
  await p.waitForSelector('.hist-item', { timeout: 8000 })
  await p.waitForTimeout(500)

  const drawer = p.locator('.el-drawer').last()
  const items = drawer.locator('.hist-item')
  const n = await items.count()
  h.check('抽屉列出会话', n >= 2, 'n=' + n)

  // 2) 列表项显示标题与条数
  const first = items.first()
  const txt = await first.innerText()
  h.check('列表显示标题', /UI 会话/.test(txt), txt.slice(0, 60))
  h.check('列表显示消息条数', /条/.test(txt), txt.slice(0, 60))

  // 3) 新会话在最前（updated_ms 倒序）
  h.check('最新会话排在最前', /UI 会话二/.test(txt), txt.slice(0, 60))

  // 4) 改名
  await first.hover()
  await first.locator('button', { hasText: '改名' }).dispatchEvent('click')
  await p.waitForSelector('.el-message-box', { timeout: 5000 })
  await p.waitForTimeout(400)
  await p.locator('.el-message-box__input input').fill('改过的会话名')
  await p.locator('.el-message-box__btns button.el-button--primary').last().dispatchEvent('click')
  await p.waitForTimeout(700)
  const afterRename = await items.first().innerText()
  h.check('改名生效', /改过的会话名/.test(afterRename), afterRename.slice(0, 60))

  // 5) 打开会话：抽屉关闭
  await items.first().locator('.hist-main').dispatchEvent('click')
  await p.waitForTimeout(700)
  await p.waitForTimeout(600)
  h.check('点击会话后抽屉收起', !(await p.locator('.hist-item').first().isVisible().catch(() => false)))

  // 6) 刷新后仍在（服务端持久化，不是本地内存）
  await p.reload()
  await p.waitForSelector('.ai-chat', { timeout: 20000 })
  await p.waitForTimeout(900)
  const cnt2 = await p.locator('.panel-head button').filter({ hasText: '历史' }).first().innerText()
  h.check('刷新后会话仍在', /（\d+）/.test(cnt2) && !/（0）/.test(cnt2), cnt2)

  // 7) 删除
  await p.locator('.panel-head button').filter({ hasText: '历史' }).first().click()
  await p.waitForSelector('.hist-item', { timeout: 8000 })
  await p.waitForTimeout(400)
  const d2 = p.locator('.el-drawer__body')
  const before = await d2.locator('.hist-item').count()
  const target = d2.locator('.hist-item', { hasText: 'UI 会话一' }).first()
  await target.hover()
  await p.waitForTimeout(300)
  // 抽屉/消息框都有过渡动画，Playwright 的 locator 会反复重试；直接在页面里点
  await p.evaluate((title) => {
    const rows = [...document.querySelectorAll('.el-drawer__body .hist-item')]
    const row = rows.find((r) => r.textContent.includes(title))
    if (!row) return false
    const btn = [...row.querySelectorAll('button')].find((b) => b.textContent.includes('删除'))
    if (!btn) return false
    btn.click()
    return true
  }, 'UI 会话一')
  await p.waitForSelector('.el-message-box', { timeout: 5000 })
  await p.waitForTimeout(400)
  await p.evaluate(() => {
    const btns = [...document.querySelectorAll('.el-message-box__btns button')]
    const ok = btns.find((b) => /删除/.test(b.textContent)) || btns[btns.length - 1]
    if (ok) ok.click()
  })
  await p.waitForTimeout(800)
  const after = await d2.locator('.hist-item').count()
  h.check('删除后少一条', after === before - 1, before + ' -> ' + after)

  // 8) 首页仪表盘也有历史入口
  await p.keyboard.press('Escape')
  await p.goto(h.A + '/#/')
  await p.waitForSelector('.ai-chat', { timeout: 20000 })
  await p.waitForTimeout(800)
  const homeHist = await p.locator('.ai-chat .panel-head button').filter({ hasText: '历史' }).count()
  h.check('首页也有历史入口', homeHist >= 1, 'n=' + homeHist)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
