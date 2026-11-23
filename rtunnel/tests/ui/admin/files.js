// 文件管理：列表视图排序、部署到设备对话框（el-dialog + el-form + 进度）。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)

  // ---- 设备页文件表排序 ----
  await h.openDevice(p, '文件')
  await p.waitForSelector('#viewToggle', { timeout: 25000 })
  await p.waitForTimeout(600)

  const toggle = p.locator('#viewToggle')
  for (let i = 0; i < 3; i++) {
    if (await p.locator('th[data-sort="size"]').count()) break
    await toggle.click()
    await p.waitForTimeout(600)
  }
  h.check('文件表格进入列表视图', (await p.locator('th[data-sort="size"]').count()) > 0)

  const names = () => p.locator('tbody tr td:nth-child(2)').allInnerTexts()
    .then((a) => a.map((x) => x.trim()).filter(Boolean))
  const clickHead = (key) => p.locator(`th[data-sort="${key}"]`).click()

  const n0 = await names()
  h.check('文件列表有数据', n0.length >= 2, n0)
  await clickHead('size')
  await p.waitForTimeout(600)
  const n1 = await names()
  h.check('点大小首次降序',
    n1[0] === 'b.txt' && n1.indexOf('a.txt') > n1.indexOf('b.txt'), n1)
  h.check('大小列显示降序箭头',
    (await p.locator('th[data-sort="size"]').innerText()).indexOf('▼') >= 0,
    await p.locator('th[data-sort="size"]').innerText())
  await clickHead('size')
  await p.waitForTimeout(600)
  const n2 = await names()
  h.check('再点大小升序', n2.indexOf('a.txt') >= 0 && n2.indexOf('a.txt') < n2.indexOf('b.txt'), n2)
  h.check('大小列显示升序箭头',
    (await p.locator('th[data-sort="size"]').innerText()).indexOf('▲') >= 0)
  await clickHead('name')
  await p.waitForTimeout(600)
  const n3 = await names()
  h.check('名称首次升序', n3.indexOf('a.txt') >= 0 && n3.indexOf('a.txt') < n3.indexOf('b.txt'), n3)

  // ---- 仓库页部署到设备 ----
  await p.goto(h.A + '/#/files')
  await p.waitForSelector('tr[data-path="/a.txt"]', { timeout: 25000 })
  await p.waitForTimeout(600)
  await p.locator('tr[data-path="/a.txt"]').click({ button: 'right' })
  await p.waitForTimeout(400)
  await p.getByText('部署到设备…', { exact: true }).click()
  await p.waitForTimeout(800)

  const dlg = p.locator('.el-dialog', { hasText: '部署到设备' })
  h.check('部署用 el-dialog', (await dlg.count()) === 1)
  h.check('对话框含 el-form', (await dlg.locator('.el-form').count()) === 1)
  h.check('对话框含 el-select', (await dlg.locator('.el-select').count()) === 1)
  h.check('对话框含 el-checkbox', (await dlg.locator('.el-checkbox').count()) === 1)

  // 明确选第一台设备，避免默认值取决于设备上线顺序
  await dlg.locator('.el-select').click()
  await p.waitForTimeout(400)
  await p.locator('.el-select-dropdown:visible .el-select-dropdown__item')
    .filter({ hasText: new RegExp('^' + h.DEV + '$') }).first().click()
  await p.waitForTimeout(900)
  h.check('目标设备已选为 ' + h.DEV,
    (await dlg.locator('.el-select').innerText()).trim().startsWith(h.DEV),
    await dlg.locator('.el-select').innerText())
  h.check('能力标签显示可写', (await dlg.innerText()).indexOf('可写') >= 0, await dlg.innerText())

  const pathInput = dlg.locator('.el-form-item', { hasText: '目标路径' }).locator('input')
  await pathInput.fill('/deployed.txt')
  await dlg.getByRole('button', { name: '部署', exact: true }).click()
  await p.waitForTimeout(2000)
  h.check('部署后对话框关闭', (await dlg.count()) === 0 || !(await dlg.isVisible()))

  // 回到设备文件列表确认落盘（目标设备是 uitest，不是仓库）
  await h.openDevice(p, '文件', h.DEV)
  await p.waitForSelector('#viewToggle', { timeout: 25000 })
  await p.waitForTimeout(1200)
  for (let i = 0; i < 3; i++) {
    if (await p.locator('tr[data-path="/deployed.txt"]').count()) break
    await p.locator('#viewToggle').click()
    await p.waitForTimeout(700)
  }
  h.check('设备文件列表出现 deployed.txt',
    (await p.locator('tr[data-path="/deployed.txt"]').count()) === 1)

  // 取消路径
  await p.goto(h.A + '/#/files')
  await p.waitForSelector('tr[data-path="/a.txt"]', { timeout: 25000 })
  await p.waitForTimeout(600)
  await p.locator('tr[data-path="/a.txt"]').click({ button: 'right' })
  await p.waitForTimeout(400)
  await p.getByText('部署到设备…', { exact: true }).click()
  await p.waitForTimeout(500)
  await dlg.getByRole('button', { name: '取消', exact: true }).click()
  await p.waitForTimeout(600)
  h.check('取消关闭对话框', !(await dlg.isVisible().catch(() => false)))

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await b.close()
  h.finish()
})()
