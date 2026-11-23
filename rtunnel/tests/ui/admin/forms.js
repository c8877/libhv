// 表单与原生组件：登录必填、移动端 drawer、账号新建校验、证书签发校验、
// 字典新增校验、权限组列表。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { ctx, page: p, errs } = await h.openPage(b, { viewport: { width: 480, height: 900 } })

  // 登录页空提交：Element 表单错误
  await p.goto(h.A + '/')
  await p.waitForSelector('#af-btn', { timeout: 20000 })
  await p.click('#af-btn')
  await p.waitForTimeout(500)
  h.check('登录空提交显示表单错误', (await p.locator('.el-form-item__error').count()) >= 2,
    await p.locator('.el-form-item__error').allInnerTexts())

  await h.login(p)

  // 移动端导航用 el-drawer
  await p.goto(h.A + '/#/agents')
  await p.waitForSelector('.hamburger', { timeout: 20000 })
  await p.locator('.hamburger').click()
  await p.waitForSelector('.el-drawer', { timeout: 8000 })
  h.check('移动菜单用 el-drawer', (await p.locator('.el-drawer .el-menu').count()) > 0)
  const item = p.locator('.el-drawer .el-menu-item', { hasText: '概览' }).first()
  h.check('抽屉里有导航项', (await item.count()) > 0)
  await item.click()
  await p.waitForTimeout(900)
  h.check('点导航后抽屉关闭', (await p.locator('.el-drawer:visible').count()) === 0)
  h.check('已跳到概览', (await p.locator('.el-descriptions').count()) > 0)

  // 账号新建：必填与最小长度
  await p.goto(h.A + '/#/accounts')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.locator('button', { hasText: '新建账号' }).first().click()
  await p.waitForSelector('.el-dialog', { timeout: 8000 })
  await p.locator('.el-dialog__footer button', { hasText: '创建' }).click()
  await p.waitForTimeout(500)
  h.check('账号空提交有必填错误',
    (await p.locator('.el-dialog .el-form-item__error').count()) >= 2,
    await p.locator('.el-dialog .el-form-item__error').allInnerTexts())
  await p.locator('.el-dialog input').nth(0).fill('uivalidate')
  await p.locator('.el-dialog input[type=password]').fill('123')
  await p.locator('.el-dialog__footer button', { hasText: '创建' }).click()
  await p.waitForTimeout(500)
  h.check('短密码提示至少 8 位',
    (await p.locator('.el-dialog .el-form-item__error').allInnerTexts()).some((t) => t.includes('8')),
    await p.locator('.el-dialog .el-form-item__error').allInnerTexts())
  await p.locator('.el-dialog__footer button', { hasText: '取消' }).click()
  await p.waitForTimeout(400)

  // 证书签发：SAN / CSR 必填，空提交不发请求
  const certPosts = []
  p.on('request', (r) => { if (r.method() === 'POST' && /certs/.test(r.url())) certPosts.push(r.url()) })
  await p.setViewportSize({ width: 1440, height: 1000 })
  await p.goto(h.A + '/#/certs')
  await p.waitForSelector('.issue-grid', { timeout: 25000 })
  await p.waitForTimeout(500)
  h.check('签发表单用 el-form', (await p.locator('.issue-grid.el-form').count()) === 1)
  await p.getByRole('button', { name: '签发', exact: true }).click()
  await p.waitForTimeout(500)
  h.check('SAN 为空显示校验错误',
    (await p.locator('.issue-grid .el-form-item__error').innerText()).includes('请填写 SAN'),
    await p.locator('.issue-grid .el-form-item__error').allInnerTexts())
  h.check('SAN 为空不发请求', certPosts.length === 0, certPosts)
  await p.locator('.issue-grid .el-form-item', { hasText: 'SAN' }).locator('input').fill('PILE-000200')
  await p.getByRole('button', { name: '签发', exact: true }).click()
  await p.waitForTimeout(600)
  h.check('填写 SAN 后通过校验并发请求', certPosts.some((u) => /certs\/issue/.test(u)), certPosts)
  await p.getByRole('button', { name: '签名', exact: true }).click()
  await p.waitForTimeout(500)
  h.check('CSR 为空显示校验错误',
    (await p.locator('.csr-item .el-form-item__error').innerText()).includes('请粘贴 CSR'),
    await p.locator('.csr-item .el-form-item__error').allInnerTexts())
  h.check('CSR 为空不发签名请求', !certPosts.some((u) => /certs\/sign/.test(u)), certPosts)

  // 字典新增：端口必填
  const dictPosts = []
  p.on('request', (r) => { if (r.method() === 'POST' && /permission\/dict/.test(r.url())) dictPosts.push(r.url()) })
  await p.goto(h.A + '/#/dicts')
  await p.waitForSelector('.el-tabs', { timeout: 25000 })
  await p.locator('.el-tabs__item', { hasText: '端口字典' }).click()
  await p.waitForTimeout(900)
  await p.getByRole('button', { name: '+ 新增', exact: true }).click()
  await p.waitForTimeout(500)
  const dlg = p.locator('.el-dialog', { hasText: '新增端口' })
  h.check('新增用 el-dialog', (await dlg.count()) === 1)
  await dlg.getByRole('button', { name: '保存', exact: true }).click()
  await p.waitForTimeout(500)
  h.check('端口为空时提示',
    (await dlg.locator('.el-form-item__error').innerText()).includes('请填写端口'),
    await dlg.locator('.el-form-item__error').allInnerTexts())
  h.check('端口为空不发请求', dictPosts.length === 0, dictPosts)
  await dlg.locator('.el-form-item', { hasText: '端口' }).locator('input').fill('9527')
  await dlg.getByRole('button', { name: '保存', exact: true }).click()
  await p.waitForTimeout(900)
  h.check('填写后提交', dictPosts.some((u) => /dict\/ports/.test(u)), dictPosts)

  // 权限组列表用 el-menu
  await p.goto(h.A + '/#/groups')
  await p.waitForSelector('.pg-list', { timeout: 25000 })
  const n = await p.locator('.pg-list .el-menu-item').count()
  h.check('权限组列表渲染', n > 0, n)
  if (n > 1) {
    await p.locator('.pg-list .el-menu-item').nth(1).click()
    await p.waitForTimeout(600)
    h.check('切换权限组高亮', (await p.locator('.pg-list .el-menu-item.is-active').count()) === 1)
  }

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await ctx.close()
  await b.close()
  h.finish()
})()
