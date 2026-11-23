// 管理台列表页表格：每页都能渲染、表头可排序、排序后行数不变、无 JS 错误。
// 另外覆盖分发 / OTA 的排序入口和设备页网卡表。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)

  const pages = [
    ['#/agents', '设备'],
    ['#/accounts', '账号'],
    ['#/tokens', 'Token'],
    ['#/access', '接入账号'],
    ['#/audit', '审计'],
    ['#/certs', '证书'],
    ['#/clients', '客户端'],
    ['#/streams', '会话'],
    ['#/subjects', '授权对象'],
    ['#/dicts', '字典'],
  ]
  for (const [route, name] of pages) {
    const before = errs.length
    await p.goto(h.A + '/' + route)
    await p.waitForTimeout(1000)
    const tables = await p.locator('table').count()
    const sortable = await p.locator('.el-table__header th.is-sortable').count()
    h.check(name + ' 表格渲染', tables > 0, tables)
    h.check(name + ' 有可排序表头', sortable > 0, sortable)
    if (sortable > 0) {
      const rows0 = await p.locator('.el-table__body tbody tr').count()
      await p.locator('.el-table__header th.is-sortable').first().click()
      await p.waitForTimeout(500)
      const rows1 = await p.locator('.el-table__body tbody tr').count()
      h.check(name + ' 点击排序后行数不变', rows0 === rows1, { rows0, rows1 })
      h.check(name + ' 排序箭头出现',
        (await p.locator('.el-table__header th.ascending, .el-table__header th.descending').count()) > 0)
    }
    h.check(name + ' 无 JS 错误', errs.length === before, errs.slice(before))
  }

  // 分发 / OTA 设备表：服务端排序（sortable="custom"）
  for (const [route, name] of [['#/distribute?file=a.txt', '分发'], ['#/ota', 'OTA 设备']]) {
    const before = errs.length
    await p.goto(h.A + '/' + route)
    await p.waitForTimeout(1500)
    const sortable = await p.locator('.el-table__header th.is-sortable').count()
    h.check(name + ' 表头可排序', sortable > 0, sortable)
    if (sortable) {
      await p.locator('.el-table__header th.is-sortable').first().click()
      await p.waitForTimeout(700)
      h.check(name + ' 排序后仍有数据',
        (await p.locator('.el-table__body tbody tr').count()) > 0)
    }
    h.check(name + ' 无 JS 错误', errs.length === before, errs.slice(before))
  }

  // 设备页网卡表：Element 本地排序
  await h.openDevice(p, '网络')
  await p.waitForSelector('.net-table', { timeout: 25000 })
  await p.waitForTimeout(800)
  const nicSortable = await p.locator('.net-table .el-table__header th.is-sortable').count()
  h.check('网卡表头可排序', nicSortable > 0, nicSortable)
  const names = () => p.locator('.net-table .el-table__body tbody tr td:nth-child(1)')
    .allInnerTexts().then((a) => a.map((x) => x.trim()))
  const n0 = await names()
  await p.locator('.net-table .el-table__header th.is-sortable', { hasText: '网卡' }).click()
  await p.waitForTimeout(600)
  const n1 = await names()
  h.check('网卡表排序生效',
    n1.length === n0.length && n1.every((x, i) => i === 0 || n1[i - 1].localeCompare(x, 'zh-CN') <= 0), n1)
  h.check('网卡表箭头出现',
    (await p.locator('.net-table .el-table__header th.ascending, .net-table .el-table__header th.descending').count()) > 0)

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await b.close()
  h.finish()
})()
