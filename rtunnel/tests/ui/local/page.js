// 设备本地页（agent 内置单文件 SPA）：9 个 tab、排序、监控、串口只读、文件编辑、存储、服务。
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b, { viewport: { width: 1280, height: 950 } })

  const apiReqs = []
  p.on('request', (r) => {
    if (/\/api\/v1\/(procs|net|metrics)/.test(r.url())) apiReqs.push(r.url())
  })
  const assets = []
  p.on('request', (r) => { if (/cm-editor/.test(r.url())) assets.push(r.url()) })

  await p.goto(h.L + '/')
  await p.waitForSelector('.dev-tabs .el-tabs__item', { timeout: 30000 })
  await p.waitForTimeout(1500)

  // 深文本：共享组件可能渲染在 shadow DOM 里；跳过 style/script，避免读到样式表
  async function deepText(sel) {
    return p.evaluate((s) => {
      const root = document.querySelector(s)
      if (!root) return 'missing'
      let out = ''
      function walk(node) {
        for (const n of node.childNodes) {
          if (n.nodeType === 3) { out += ' ' + n.nodeValue; continue }
          if (n.nodeType !== 1) continue
          const tag = n.tagName
          if (tag === 'STYLE' || tag === 'SCRIPT') continue
          if (n.shadowRoot) walk(n.shadowRoot)
          walk(n)
        }
      }
      walk(root)
      return out.replace(/\s+/g, ' ').trim().slice(0, 400)
    }, sel)
  }
  const tab = async (name) => {
    await p.locator('.dev-tabs .el-tabs__item', { hasText: name }).click()
    await p.waitForTimeout(1200)
  }

  const tabs = await p.locator('.dev-tabs .el-tabs__item').allInnerTexts()
  h.check('本地页九个 tab', tabs.length === 9 && tabs[0] === '概览' && tabs[8] === '能力', tabs)
  h.check('本地页四个 KPI', (await p.locator('.ov-kpi').count()) === 4)
  const ovText = await p.locator('.ov-wrap').innerText()
  h.check('本地页不显示 relay 会话卡', !ovText.includes('会话'), ovText.slice(0, 200))

  // ---- 进程：服务端排序 ----
  await tab('进程')
  await p.waitForSelector('.feat-panel .el-table__body tbody tr', { timeout: 30000 })
  const cpu = p.locator('.feat-panel .el-table__header th', { hasText: 'CPU' })
  h.check('进程默认 CPU 降序', ((await cpu.getAttribute('class')) || '').indexOf('descending') >= 0,
    await cpu.getAttribute('class'))
  const [procResp] = await Promise.all([
    p.waitForResponse((r) => /\/procs\?/.test(r.url()) && r.url().includes('dir=1'), { timeout: 30000 }),
    cpu.click({ position: { x: 6, y: 8 } }),
  ])
  const procJson = await procResp.json()
  h.check('本地进程点击后升序请求', h.sorted(procJson.entries.map((x) => x.cpu_pct), 1))
  h.check('本地进程箭头变升序', ((await cpu.getAttribute('class')) || '').indexOf('ascending') >= 0,
    await cpu.getAttribute('class'))

  // ---- 网络：TCP 本地端口降序 ----
  await tab('网络')
  await p.locator('.net-tabs .el-tabs__item', { hasText: 'TCP' }).click()
  await p.waitForSelector('.net-table .el-table__body tbody tr', { timeout: 40000 })
  await p.waitForTimeout(600)
  const portTh = p.locator('.net-table .el-table__header th', { hasText: '本地端口' })
  const [netResp] = await Promise.all([
    p.waitForResponse((r) => /\/net\?/.test(r.url()) && r.url().includes('dir=-1'), { timeout: 40000 }),
    portTh.click({ position: { x: 6, y: 8 } }),
  ])
  const netJson = await netResp.json()
  const ports = (netJson.tcp.entries || []).map((x) => x.local_port)
  h.check('本地 TCP 本地端口降序', ports.length > 1 && h.sorted(ports, -1), ports.slice(0, 6))
  const domPorts = (await p.locator('.net-table .el-table__body tbody tr td:nth-child(2)').allInnerTexts())
    .map((x) => Number(x.trim()))
  h.check('本地 TCP 表格顺序一致', h.sorted(domPorts, -1), domPorts.slice(0, 6))
  await p.locator('.net-tabs .el-tabs__item', { hasText: 'UNIX' }).click()
  await p.waitForSelector('.net-table .el-table__body tbody tr', { timeout: 40000 })
  h.check('本地 UNIX 表有数据', (await p.locator('.net-table .el-table__body tbody tr').count()) > 0)

  // ---- 监控 ----
  await tab('监控')
  await p.waitForTimeout(2500)
  h.check('本地监控有曲线', (await p.locator('.chart-box canvas').count()) > 0)
  const chips = await p.locator('.series-select .el-tag').allInnerTexts()
  h.check('本地进程 chip 默认选中', chips.some((t) => /(\[1\]|PID 1)/.test(t)), chips)

  // ---- 串口：只读 ----
  await tab('串口')
  const ttyText = await p.locator('.obs-panel').innerText()
  h.check('串口页可打开', ttyText.includes('已配置') || ttyText.includes('暂无') || ttyText.includes('串口'),
    ttyText.slice(0, 100))
  h.check('本地页隐藏串口编辑', (await p.locator('.el-button', { hasText: '手动添加' }).count()) === 0)
  h.check('本地页隐藏串口调试', (await p.locator('.el-button', { hasText: '调试' }).count()) === 0)

  // ---- 能力：只读 ----
  await tab('能力')
  await p.waitForTimeout(600)
  h.check('本地页能力只读', (await p.locator('.caps-runtime button', { hasText: '编辑配置' }).count()) === 0)
  h.check('本地页无能力编辑表单', (await p.locator('.caps-grid').count()) === 0)

  // ---- 文件：列表 + 编辑器 ----
  await tab('文件')
  await p.waitForTimeout(2500)
  h.check('文件管理器已挂载', (await p.locator('.fs-host').count()) === 1)
  const fsText = await deepText('.fs-host')
  h.check('文件列表含 a.txt', fsText.includes('a.txt'), fsText.slice(0, 160))
  // 右键菜单里「编辑（文本）」才会加载 CodeMirror
  await p.locator('.fs-host tr[data-path="/a.txt"]').first().click({ button: 'right' })
  await p.waitForTimeout(500)
  await p.getByText('编辑（文本）', { exact: true }).click()
  await p.waitForTimeout(3500)
  h.check('打开文件后加载编辑器', (await p.locator('.cm-editor, .CodeMirror').count()) > 0)
  h.check('编辑器脚本按需加载', assets.length > 0, assets)
  // 编辑器是模态面板，先关掉再切 tab
  await p.locator('#edClose').click()
  await p.waitForTimeout(600)
  h.check('编辑器可关闭', (await p.locator('.cm-editor, .CodeMirror').count()) === 0)

  // ---- 存储 ----
  await tab('存储')
  await p.waitForTimeout(4500)
  h.check('存储页有挂载点', (await p.locator('.feat-panel .store-head').count()) >= 1)
  h.check('存储树已渲染', (await deepText('.store-host')).length > 10, (await deepText('.store-host')).slice(0, 120))

  // ---- 服务 ----
  await tab('服务')
  await p.waitForTimeout(3000)
  const svcText = await deepText('.svc-host')
  h.check('服务页已挂载', svcText !== 'missing', svcText.slice(0, 120))

  h.check('本地页无 JS 错误', errs.length === 0, errs.slice(0, 4))
  await h.shot(p, 'local-page')
  await b.close()
  h.finish()
})()
