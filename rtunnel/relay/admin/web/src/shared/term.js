// 终端会话：跨路由保留 xterm 实例与 WebSocket
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'

const sessions = new Map()
let seq = 0

export const TermMgr = {
  onChange: null,

  open(devid, fontSize) {
    const id = devid + '#' + (++seq)
    const box = document.createElement('div')
    box.className = 'term-box'

    const term = new Terminal({
      cursorBlink: true,
      fontSize: fontSize || 14,
      theme: { background: '#1e1e1e' },
    })
    const fit = new FitAddon()
    term.loadAddon(fit)

    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(proto + '://' + location.host + '/ws/shell?devid=' + encodeURIComponent(devid))
    const s = { id, devid, term, fit, ws, box, opened: false, alive: true }
    ws.onmessage = (e) => term.write(e.data)
    ws.onclose = () => {
      s.alive = false
      try { term.write('\r\n[连接已关闭]\r\n') } catch (e) { /* 已销毁 */ }
      if (TermMgr.onChange) TermMgr.onChange()
    }
    term.onData((d) => { if (ws.readyState === 1) ws.send(d) })
    term.onResize(({ cols, rows }) => {
      if (ws.readyState === 1) ws.send(JSON.stringify({ type: 'resize', cols, rows }))
    })

    sessions.set(id, s)
    if (TermMgr.onChange) TermMgr.onChange()
    return s
  },

  aliveOf(devid) {
    for (const s of sessions.values()) if (s.devid === devid && s.alive) return s
    return null
  },

  get(id) {
    return sessions.get(id)
  },

  list() {
    return Array.from(sessions.values())
  },

  close(id) {
    const s = sessions.get(id)
    if (!s) return
    s.alive = false
    try { s.ws.close() } catch (e) { /* 已关闭 */ }
    try { s.term.dispose() } catch (e) { /* 已销毁 */ }
    s.box.remove()
    sessions.delete(id)
    if (TermMgr.onChange) TermMgr.onChange()
  },
}
