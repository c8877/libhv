// AI 助手页：未配置引导、模型设置弹窗（服务商/模型/Key）、空状态、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/assistant')
  await p.waitForSelector('.ai-chat', { timeout: 20000 })
  await p.waitForTimeout(600)
  h.check('页面标题', (await p.locator('.panel-head h2').first().innerText()) === 'AI 助手')

  // 没配置时应有引导块，而不是空白输入框
  const hasSetup = (await p.locator('.setup').count()) > 0
  h.check('未配置时给出引导', hasSetup)
  if (hasSetup) {
    const t = await p.locator('.setup').innerText()
    h.check('引导说明可读', /服务商|API Key|配置/.test(t), t.slice(0, 80))
  }

  // 空状态：建议问题 + 提议说明
  const chips = await p.locator('.chip').count()
  h.check('空状态有建议问题', chips >= 3, 'chips=' + chips)
  const emptyTip = await p.locator('.empty-tip').innerText()
  h.check('空状态说明会生成待确认作业', /作业卡|确认|只读/.test(emptyTip), emptyTip)

  // 建议问题只填输入框、不发送（未配置时发送应被拦）
  await p.locator('.chip').first().click()
  await p.waitForTimeout(400)
  const ta = p.locator('.composer textarea')
  h.check('点击建议填入输入框', (await ta.inputValue()).length > 0, await ta.inputValue())
  const msgsBefore = await p.locator('.msg').count()
  h.check('未配置时不产生对话', msgsBefore === 0, 'msgs=' + msgsBefore)

  // 设备上下文下拉存在
  h.check('有设备上下文选择', (await p.locator('.panel-head .el-select').count()) >= 1)

  // 模型设置弹窗：只有服务商/模型/Key 三个必填项，高级默认收起
  // 页面加载完会再渲染一次，等它稳定再点，否则 Playwright 报 element is not stable
  await p.waitForTimeout(900)
  // 模型设置不常改，收在头部的「更多」下拉里；先展开再点菜单项
  // 点完会立刻弹层，Playwright 的 click 自检会被浮层挡住；直接派发 click
  await p.locator('.panel-head .more-btn').last().dispatchEvent('click')
  await p.waitForSelector('.el-dropdown-menu:visible', { timeout: 5000 })
  await p.locator('.el-dropdown-menu:visible .el-dropdown-menu__item', { hasText: '模型设置' }).click()
  await p.waitForSelector('.el-dialog:visible', { timeout: 5000 })
  await p.waitForTimeout(700)   // 等 dialog 动画结束，否则 Playwright 报 element is not stable
  const dlg = p.locator('.el-dialog:visible')
  const dtext = await dlg.innerText()
  h.check('弹窗只有核心项', /服务商/.test(dtext) && /模型/.test(dtext) && /API Key/.test(dtext), dtext.slice(0, 80))
  h.check('默认不展开高级', (await dlg.locator('input[placeholder*="完整的"]').count()) === 0,
    'endpoint 输入框不该一开始就出现')
  h.check('高级入口存在', (await dlg.locator('button').filter({ hasText: '高级' }).count()) === 1)

  // 切服务商应自动带出默认模型
  await dlg.locator('.el-select__wrapper').first().dispatchEvent('click')
  await p.waitForTimeout(400)
  const opt = p.locator('.el-select-dropdown:visible .el-select-dropdown__item')
    .filter({ hasText: 'Kimi' }).first()
  await opt.waitFor({ timeout: 5000 })
  await opt.dispatchEvent('click')
  await p.waitForTimeout(300)
  const modelVal = await dlg.locator('.el-form-item', { hasText: '模型' }).first().locator('input').inputValue()
  h.check('切服务商自动填模型', modelVal.includes('moonshot'), modelVal)

  // 展开高级才看得到接口地址
  await dlg.locator('button').filter({ hasText: '高级' }).first().dispatchEvent('click')
  await p.waitForTimeout(300)
  h.check('高级里有接口地址', (await dlg.innerText()).includes('接口地址'))

  await dlg.getByRole('button', { name: '取消' }).dispatchEvent('click')
  await p.waitForTimeout(300)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
