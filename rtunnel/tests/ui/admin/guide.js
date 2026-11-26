// 使用指南页：章节齐全、目录可跳转、代码块可复制、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/guide')
  await p.waitForSelector('.guide', { timeout: 20000 })
  await p.waitForTimeout(400)

  h.check('页面标题', (await p.locator('.panel-head h2').first().innerText()) === '使用指南')

  const secs = await p.locator('.sec').count()
  h.check('章节数量', secs >= 6, 'sections=' + secs)

  const toc = await p.locator('.toc-item').count()
  h.check('目录条目与章节一致', toc === secs, toc + ' vs ' + secs)

  const text = await p.locator('.guide').innerText()
  for (const k of ['部署 relay', '接入设备', '权限组', '日常运维', '接入大模型', '常见问题']) {
    h.check('包含章节: ' + k, text.includes(k))
  }
  h.check('提到密钥只落文件', text.includes('ai.key') || text.includes('不进数据库'),
    text.slice(0, 60))

  // 目录跳转：点“常见问题”后该节进入视口
  await p.locator('.toc-item', { hasText: '常见问题' }).click()
  await p.waitForTimeout(800)
  const inView = await p.evaluate(() => {
    const el = document.getElementById('faq')
    if (!el) return false
    const r = el.getBoundingClientRect()
    return r.top < window.innerHeight && r.bottom > 0
  })
  h.check('目录可跳转到章节', inView)

  // 代码块有复制按钮
  const copies = await p.locator('.code-wrap .copy').count()
  h.check('代码块带复制按钮', copies >= 3, 'copies=' + copies)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
