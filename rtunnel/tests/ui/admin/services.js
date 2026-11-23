// 服务管理：重启次数列排序（首次降序），依赖宿主 systemd。
// 没有 systemd 或没有 unit 的环境自动跳过排序断言。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)
  await h.openDevice(p, '服务')
  await p.waitForTimeout(4500)

  const hasService = (await p.locator('.feat-panel table tbody tr').count()) > 0
  const heads = await p.locator('.feat-panel table thead th').allInnerTexts()
  const errText = (await p.locator('.feat-panel .err').allInnerTexts()).join(' ')
  if (!hasService) {
    h.skip('服务重启列排序', '宿主没有可枚举的 systemd 服务' + (errText ? '：' + errText.slice(0, 80) : ''))
  } else {
    h.check('表头有重启列', heads.some((x) => x.trim().startsWith('重启')), heads)
    const cells = await p.locator('.feat-panel table tbody tr td.c-rest')
      .evaluateAll((els) => els.map((e) => e.textContent.trim()))
    h.check('重启列有值',
      cells.length > 0 && cells.every((t) => /^(\d+|—)$/.test(t)), cells.slice(0, 8))

    // 数值列首次点击降序，再点升序
    await p.locator('.feat-panel table thead th', { hasText: '重启' }).click()
    await p.waitForSelector('.feat-panel table tbody tr', { timeout: 20000 })
    await p.waitForTimeout(1800)
    const desc = await p.locator('.feat-panel table tbody tr td.c-rest')
      .evaluateAll((els) => els.map((e) => e.textContent.trim()))
    const nums = desc.map((t) => (t === '—' ? 0 : Number(t)))
    h.check('降序单调不增', nums.every((n, i) => i === 0 || nums[i - 1] >= n), nums.slice(0, 8))
    h.check('重启列箭头向下',
      (await p.locator('.feat-panel table thead th', { hasText: '重启' }).innerText()).indexOf('▼') >= 0)
    await p.locator('.feat-panel table thead th', { hasText: '重启' }).click()
    await p.waitForTimeout(1800)
    const asc = await p.locator('.feat-panel table tbody tr td.c-rest')
      .evaluateAll((els) => els.map((e) => e.textContent.trim()))
    const nums2 = asc.map((t) => (t === '—' ? 0 : Number(t)))
    h.check('再点重启升序', nums2.every((n, i) => i === 0 || nums2[i - 1] <= n), nums2.slice(0, 8))
  }

  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))
  await b.close()
  h.finish()
})()
