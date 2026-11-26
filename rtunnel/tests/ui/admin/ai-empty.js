// AI 助手：接口失败且没有正文时，气泡要有可读的错误，不能只剩“这里还没有内容”
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1280, height: 900 } })
  await h.login(p)

  // 模型配置假装就绪；对话接口固定回一个“没有正文”的失败
  await p.route('**/api/v1/ai/config', (route) => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({
      ready: true, subject: 'u-1', provider: 'deepseek', model: 'mock',
      providers: [{ key: 'deepseek', label: 'DeepSeek', model: 'mock' }],
      key_set: true, tools: 21, tools_total: 21,
    }),
  }))
  await p.route('**/api/v1/ai/chat', (route) => route.fulfill({
    status: 502,
    contentType: 'application/json',
    body: JSON.stringify({ error: '模型没有返回可读的回答（已调用 7 轮）' }),
  }))

  await p.goto(h.A + '/#/assistant')
  await p.waitForSelector('.ai-chat', { timeout: 20000 })
  await p.waitForTimeout(700)

  await p.locator('.composer textarea').fill('你能部署内网指定主机的 rtunnel 应用吗')
  await p.locator('.composer .send-btn').click()
  await p.waitForSelector('.msg.assistant', { timeout: 8000 })
  await p.waitForTimeout(600)

  const txt = await p.locator('.msg.assistant .bubble').first().innerText()
  h.check('失败时气泡有可读文字', txt.includes('模型没有返回可读的回答'), txt)
  h.check('失败时不再显示空占位', !txt.includes('这里还没有内容'), txt)
  h.check('失败时不渲染空 Markdown 块', (await p.locator('.ai-chat .md-empty').count()) === 0)
  h.check('用户的问题仍在气泡里', (await p.locator('.msg.user .bubble').first().innerText()).includes('rtunnel'))

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
