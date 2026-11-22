// 设备本地页 API 适配：把管理台的 /api/v1/agents/<devid>/* 映射到 agent
// 自己提供的 /api/v1/*。只有 DeviceView 共用的那几个接口，缺少的读写能力
// 明确报错，按钮由资源权限隐藏。
import { setApiAdapter } from '@/shared/api.js'

// 本地页可能被 relay 以 /device/<devid>/<port>/ 前缀代理，接口必须走相对路径
function apiUrl(path) {
  const base = location.pathname.replace(/\/?$/, '/')
  return base + String(path).replace(/^\/+/, '')
}

async function jget(path) {
  const r = await fetch(apiUrl(path), {
    cache: 'no-store',
    headers: { 'X-Requested-With': 'fetch' },
  })
  if (!r.ok) {
    let msg = 'HTTP ' + r.status
    try {
      const j = await r.json()
      if (j && j.error) msg = j.error
    } catch (e) { /* 非 JSON 错误体 */ }
    const err = new Error(msg)
    err.status = r.status
    throw err
  }
  try {
    return await r.json()
  } catch (e) {
    return null
  }
}

// sysinfo 是一次几百 KB 的采集，短时间内多处读取共用一份
let sysCache = { at: 0, data: null }

async function sysinfo(force) {
  if (!force && sysCache.data && Date.now() - sysCache.at < 800) return sysCache.data
  const d = await jget('api/v1/sysinfo')
  if (d && d.caps && !d.caps.version && d.version) d.caps.version = d.version
  sysCache = { at: Date.now(), data: d }
  return d
}

function serialList(d) {
  const arr = (d && d.serial) || []
  return arr.map((s) => ({
    path: s.path,
    alias: s.alias || '',
    configured: !!s.configured,
    enabled: !!s.enabled,
    baud: Number(s.baud) || 115200,
    data_bits: Number(s.data_bits) || 8,
    parity: s.parity || 'N',
    stop_bits: Number(s.stop_bits) || 1,
    roles: s.roles || 'operator,manager,admin',
    rx: Number(s.rx) || 0,
    tx: Number(s.tx) || 0,
  }))
}

function metricConfig(d) {
  const cfg = (d && d.caps && d.caps.metrics && d.caps.metrics.series) || []
  const list = cfg.length ? cfg : ['cpu', 'mem', 'load'].map((s) => ({ series: s, interval_sec: 5, points: 720 }))
  return list.map((c) => ({
    series: c.series,
    enabled: c.enabled !== false,
    interval_sec: Number(c.interval_sec) || 5,
    points: Number(c.points) || 720,
    synced: true,
  }))
}

export function installLocalApi() {
  setApiAdapter(async (path, opt = {}) => {
    const method = String(opt.method || 'GET').toUpperCase()

    // 串口/端口字典由管理台维护，本地页没有字典库
    if (path.startsWith('/api/v1/permission/dicts')) {
      return { items: [], total: 0, page: 1, page_size: 200 }
    }

    const m = path.match(/^\/api\/v1\/agents\/[^/?#]+(\/[^?]*)?(\?.*)?$/)
    if (!m) throw new Error('本地页面不支持该接口：' + path)
    const suffix = m[1] || ''
    const query = m[2] || ''

    if (method !== 'GET') throw new Error('本地页面不支持修改该配置')

    // 管理台的 detail 是 relay 会话信息；本地页没有 relay，头部改用 sysinfo 展示
    if (!suffix || suffix === '/') return null
    if (suffix === '/sysinfo') {
      const d = await sysinfo(true)
      return { ts: Date.now(), mode: 'full', cached: false, sysinfo: d }
    }
    if (suffix === '/procs') return jget('api/v1/procs' + query)
    if (suffix.startsWith('/procs/')) {
      const pid = decodeURIComponent(suffix.slice('/procs/'.length))
      const r = await jget('api/v1/procs?page=1&page_size=50&q=' + encodeURIComponent(pid))
      const row = ((r && r.entries) || []).find((e) => String(e.pid) === String(pid))
      if (!row) throw new Error('进程不存在或已退出')
      return Object.assign({
        ppid: 0, uid: 0, user: '', state: '', state_text: '',
        vsz_kb: 0, vmpeak_kb: 0, vmswap_kb: 0, cpu_sec: 0,
        exe: '', cwd: '', cmd: '',
      }, row)
    }
    if (suffix === '/net') return jget('api/v1/net' + query)
    if (suffix === '/storage') return jget('api/v1/storage/tree' + query)
    if (suffix === '/metrics/series') return jget('api/v1/metrics' + query)
    if (suffix === '/metrics/series/config') return metricConfig(await sysinfo())
    if (suffix === '/ttys') {
      const d = await sysinfo()
      return serialList(d).filter((s) => s.configured)
    }
    if (suffix === '/ttys/discovered') {
      const d = await sysinfo()
      return serialList(d).filter((s) => !s.configured)
    }
    if (suffix === '/caps') {
      const d = await sysinfo()
      return (d && d.caps) || {}
    }
    throw new Error('本地页面不支持该操作')
  })
}
