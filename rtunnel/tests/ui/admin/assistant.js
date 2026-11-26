// AI 助手页：未配置时的引导、空状态建议、发送拦截、无 JS 错误
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1440, height: 1000 } })
  await h.login(p)

  await p.goto(h.A + '/#/assistant')
  await p.waitForSelector('.chat-panel', { timeout: 20000 })
  await p.waitForTimeout(600)
  h.check('页面标题', (await p.locator('.panel-head h2').first().innerText()) === 'AI 助手')

  // 没配密钥 / 没开开关时应有引导块，而不是空白输入框
  const hasSetup = (await p.locator('.setup').count()) > 0
  h.check('未配置时给出引导', hasSetup)
  if (hasSetup) {
    const t = await p.locator('.setup').innerText()
    h.check('引导说明可读', /停用|密钥|配置/.test(t), t.slice(0, 80))
  }

  // 空状态：建议问题 + 只读提示
  const chips = await p.locator('.chip').count()
  h.check('空状态有建议问题', chips >= 3, 'chips=' + chips)
  const emptyTip = await p.locator('.empty-tip').innerText()
  h.check('空状态说明只读', emptyTip.includes('只读'), emptyTip)

  // 建议问题只填输入框、不发送（未配置时发送应被拦）
  await p.locator('.chip').first().click()
  await p.waitForTimeout(400)
  const ta = p.locator('.composer textarea')
  h.check('点击建议填入输入框', (await ta.inputValue()).length > 0, await ta.inputValue())

  const msgsBefore = await p.locator('.msg').count()
  h.check('未配置时不产生对话', msgsBefore === 0, 'msgs=' + msgsBefore)

  // 设备上下文下拉存在
  h.check('有设备上下文选择', (await p.locator('.panel-head .el-select').count()) >= 1)

  h.check('无 JS 错误', errs.length === 0, errs)
  await b.close()
  h.finish()
})()
