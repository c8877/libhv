// 运维 → 中继列表：地址字典、下发顺序、保存列表、同步任务创建与详情
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { ctx, page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  // 菜单入口在「运维」组（默认折叠，只校验已注册）
  await p.goto(h.A + '/#/relay-list')
  await p.waitForSelector('.panel', { timeout: 20000 })
  await p.waitForTimeout(600)
  h.check('运维组有中继列表菜单',
    (await p.locator('.el-menu-item', { hasText: '中继列表' }).count()) > 0)
  h.check('打开中继列表页', p.url().endsWith('/relay-list'), p.url())

  // 新增两条地址
  async function add(host, port, name) {
    await p.locator('.panel button', { hasText: '新增地址' }).first().click()
    await p.waitForSelector('.el-dialog', { timeout: 8000 })
    await p.locator('.el-dialog input').nth(0).fill(host)
    const portBox = p.locator('.el-dialog .el-input-number input')
    await portBox.fill(String(port))
    await p.locator('.el-dialog input').nth(2).fill(name)
    await p.locator('.el-dialog__footer button', { hasText: '保存' }).click()
    await p.waitForTimeout(600)
  }
  h.check('初始为空', (await p.locator('.el-table__row:visible').count()) === 0)
  await add('10.1.1.1', 18443, '主 relay')
  await add('10.1.1.2', 18443, '备 relay')
  await p.waitForTimeout(400)
  h.check('新增两条地址', (await p.locator('.el-table__row:visible').count()) === 2,
    await p.locator('.el-table__row:visible').allInnerTexts())

  // 表单校验：地址必填
  await p.locator('.panel button', { hasText: '新增地址' }).first().click()
  await p.waitForSelector('.el-dialog', { timeout: 8000 })
  await p.locator('.el-dialog__footer button', { hasText: '保存' }).click()
  await p.waitForTimeout(500)
  h.check('地址为空显示校验错误',
    (await p.locator('.el-dialog .el-form-item__error').count()) >= 1,
    await p.locator('.el-dialog .el-form-item__error').allInnerTexts())
  await p.locator('.el-dialog__footer button', { hasText: '取消' }).click()
  await p.waitForTimeout(400)

  // 勾选加入下发列表 → 顺序列出现序号
  await p.locator('.el-table__row:visible').nth(0).locator('.el-checkbox').click()
  await p.waitForTimeout(300)
  await p.locator('button', { hasText: '加入下发列表' }).click()
  await p.waitForTimeout(400)
  h.check('加入后出现顺序 1', (await p.locator('.el-table__row:visible').nth(0).locator('.pos').innerText()) === '1')
  h.check('未保存标记', (await p.locator('.panel-head .el-tag', { hasText: '未保存' }).count()) === 1)

  // 保存列表 → 状态变已启用
  await p.locator('.panel-head button', { hasText: '保存列表' }).click()
  await p.waitForTimeout(800)
  h.check('保存后状态为已启用',
    (await p.locator('.panel-head').first().innerText()).includes('已启用'))
  h.check('保存后不再有未保存标记',
    (await p.locator('.panel-head .el-tag', { hasText: '未保存' }).count()) === 0)

  // 设备页：策略表单与设备表
  await p.locator('.el-tabs__item', { hasText: '设备' }).first().click()
  await p.waitForTimeout(800)
  h.check('有下发方式切换', (await p.locator('.el-radio-button', { hasText: '分批灰度' }).count()) === 1)
  await p.locator('.el-radio-button', { hasText: '分批灰度' }).click()
  await p.waitForTimeout(300)
  h.check('分批模式出现每批台数', (await p.locator('.el-form-item', { hasText: '每批台数' }).count()) === 1)
  const rows = p.locator('.el-table__row:visible')
  h.check('设备表有测试设备', (await rows.count()) >= 2, await rows.allInnerTexts())

  // 选两台设备创建任务
  await rows.nth(0).locator('.el-checkbox').click()
  await rows.nth(1).locator('.el-checkbox').click()
  await p.waitForTimeout(300)
  h.check('已选两台', (await p.locator('.bar .muted', { hasText: '已选' }).first().innerText()).includes('已选 2'))
  await p.locator('button', { hasText: '创建同步任务' }).click()
  await p.waitForTimeout(1500)
  const taskPanel = p.locator('.tasks')
  h.check('同步任务表出现记录', (await taskPanel.locator('.el-table__row').count()) >= 1,
    await taskPanel.locator('.el-table__row').allInnerTexts())
  const rowText = await taskPanel.locator('.el-table__row').first().innerText()
  h.check('任务显示批次信息', /1\/1|1\/2/.test(rowText), rowText)

  // 详情抽屉
  await taskPanel.locator('button', { hasText: '详情' }).first().click()
  await p.waitForSelector('.el-drawer:visible', { timeout: 8000 })
  await p.waitForTimeout(600)
  const drawer = p.locator('.el-drawer:visible').last()
  h.check('抽屉有任务号', (await drawer.innerText()).includes('任务号'))
  h.check('抽屉逐设备明细', (await drawer.locator('.el-table__row').count()) === 2)
  await p.keyboard.press('Escape')
  await p.waitForTimeout(400)

  // 刷新后列表顺序保持（落库）
  await p.reload()
  await p.waitForTimeout(1200)
  h.check('刷新后仍显示已启用', (await p.locator('.panel-head').first().innerText()).includes('已启用'))

  h.check('无 JS 错误', errs.length === 0, errs)
  await h.shot(p, 'admin-relay-list')
  await b.close()
  h.finish()
})()
