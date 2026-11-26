// 使用指南页：由 src/docs/guide.md 渲染，验证 markdown 组件的标题/目录/代码/表格/站内链接
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/guide')
  await p.waitForSelector('.md-body', { timeout: 20000 })
  await p.waitForTimeout(500)

  h.check('页面标题', (await p.locator('.panel-head h2').first().innerText()) === '使用指南')

  const h2s = await p.locator('.md-body h2').count()
  h.check('章节数量', h2s >= 6, 'h2=' + h2s)

  const toc = await p.locator('.toc-item').count()
  h.check('目录条目与章节一致', toc === h2s, toc + ' vs ' + h2s)

  const text = await p.locator('.md-body').innerText()
  for (const k of ['部署 relay', '接入设备', '权限组', '日常运维', '接入大模型', '常见问题']) {
    h.check('包含章节: ' + k, text.includes(k))
  }
  const strong = await p.locator('.md-body strong').count()
  h.check('强调被渲染（不是字面量 **）', strong > 0 && !text.includes('**'), 'strong=' + strong)

  // 目录跳转：点“常见问题”后该节进入视口（锚点来自 {#faq}）
  await p.locator('.toc-item', { hasText: '常见问题' }).click()
  await p.waitForTimeout(900)
  const inView = await p.evaluate(() => {
    const el = document.getElementById('faq')
    if (!el) return false
    const r = el.getBoundingClientRect()
    return r.top < window.innerHeight && r.bottom > 0
  })
  h.check('目录可跳转到章节', inView)

  // 代码块：语言标签 + 复制按钮
  const codes = await p.locator('.md-code').count()
  const copies = await p.locator('.md-code .md-copy').count()
  h.check('代码块带复制按钮', codes >= 3 && copies === codes, 'codes=' + codes + ' copies=' + copies)
  const bash = await p.locator('.md-code[data-lang="bash"]').count()
  h.check('代码块显示语言', bash >= 3, 'bash blocks=' + bash)

  // 表格：常见问题是一张表
  const rows = await p.locator('.md-table table tr').count()
  h.check('表格已渲染', rows >= 4, 'rows=' + rows)
  h.check('表头不为空', (await p.locator('.md-table th').first().innerText()).length > 0)

  // 站内链接：文档里写 [权限组](#/groups) 应直接跳页面
  await p.locator('.md-body a[href="#/groups"]').first().click()
  await p.waitForTimeout(600)
  h.check('站内链接可跳转', p.url().includes('/groups'), p.url())

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
