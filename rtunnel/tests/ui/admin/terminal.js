// Web 终端：管理台内部代理认证 + 设备 shell 打通（/ws/shell）
const h = require('../lib/harness')

;(async () => {
  const b = await h.launch()
  const { page: p, errs } = await h.openPage(b)
  await h.login(p)

  const wsUrl = h.A.replace(/^http/, 'ws') + '/ws/shell?devid=' + encodeURIComponent(h.DEV)
  const res = await p.evaluate((url) => new Promise((resolve) => {
    const ws = new WebSocket(url)
    const out = { opened: false, closed: false, code: 0, data: '' }
    ws.onopen = () => { out.opened = true; ws.send('\r') }
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') out.data += e.data.slice(0, 80)
    }
    ws.onclose = (e) => { out.closed = true; out.code = e.code }
    // 先确认能收到数据，再让 shell 自己退出，避免残留进程拖住清理
    setTimeout(() => {
      out.aliveBeforeExit = !out.closed && ws.readyState === 1
      try { ws.send('exit\r') } catch (e) { /* ignore */ }
    }, 3000)
    setTimeout(() => {
      try { ws.close() } catch (e) { /* ignore */ }
      resolve(out)
    }, 4000)
  }), wsUrl)

  h.check('Web 终端建立并保持', res.opened && res.aliveBeforeExit, res)
  h.check('Web 终端有输出', res.data.length > 0, res.data.slice(0, 80))
  h.check('无 JS 错误', errs.length === 0, errs.slice(0, 3))

  await b.close()
  h.finish()
})()
