// 概览 → 实时事件面板：SSE 连上、收到审计事件、清空、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(1000)

  const panel = p.locator('.panel').filter({
    has: p.locator('.panel-head h2', { hasText: '实时事件' }),
  })
  h.check('概览有实时事件面板', (await panel.count()) === 1)

  const head = await panel.locator('.panel-head').innerText()
  h.check('面板标题与提示可见', head.includes('实时事件') && head.includes('新事件在上'), head)

  // SSE 连接状态：hello 到达后显示「已连接」
  await p.waitForFunction(
    () => {
      const el = Array.from(document.querySelectorAll('.panel'))
        .find((x) => (x.querySelector('.panel-head h2') || {}).textContent === '实时事件')
      return el && el.textContent.includes('已连接')
    },
    { timeout: 10000 },
  )
  h.check('事件流已连接', true)

  // 通过管理 API 改一个设置（会写审计）→ 面板应出现「审计」条目
  await p.evaluate(async () => {
    await fetch('/api/v1/settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ 'alert.cooldown_s': '307' }),
    })
  })
  const row = panel.locator('.evt', { hasText: '审计' }).first()
  await row.waitFor({ timeout: 8000 })
  const txt = await row.innerText()
  h.check('收到审计事件', txt.includes('settings.update') || txt.includes('审计'), txt)

  // 清空
  await panel.locator('button', { hasText: '清空' }).click()
  await p.waitForTimeout(300)
  h.check('清空后回到空状态', (await panel.locator('.evt').count()) === 0)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
