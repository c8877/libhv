import { ElMessage } from 'element-plus'

export function bytes(n, iec = false) {
  n = Number(n) || 0
  const units = iec
    ? ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB']
    : ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
  let i = 0
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024
    i++
  }
  const digits = i === 0 ? 0 : n >= 100 ? 0 : n >= 10 ? 1 : 2
  return n.toFixed(digits) + ' ' + units[i]
}

// 1024 进制、最多 1 位小数；用量/费用/租户页共用这套口径
export function bytesIec(n) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let v = Number(n) || 0
  let i = 0
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return (i === 0 ? String(v) : v.toFixed(v >= 100 ? 0 : 1)) + ' ' + units[i]
}

// 秒数 → "1天2小时" / "3分4秒"，用量与费用页共用
export function spanText(seconds) {
  let s = Math.round(Number(seconds) || 0)
  const d = Math.floor(s / 86400); s -= d * 86400
  const h = Math.floor(s / 3600); s -= h * 3600
  const m = Math.floor(s / 60); s -= m * 60
  if (d) return `${d}天${h}小时`
  if (h) return `${h}小时${m}分`
  if (m) return `${m}分${s}秒`
  return `${s}秒`
}

export function duration(ms) {
  let s = Math.max(0, Math.floor((Number(ms) || 0) / 1000))
  const d = Math.floor(s / 86400)
  s %= 86400
  const h = Math.floor(s / 3600)
  s %= 3600
  const m = Math.floor(s / 60)
  s %= 60
  if (d) return d + '天 ' + h + '小时'
  if (h) return h + '小时 ' + m + '分'
  if (m) return m + '分 ' + s + '秒'
  return s + '秒'
}

export function ago(ms) {
  const t = Number(ms) || 0
  if (!t) return '—'
  const diff = Math.max(0, Date.now() - t)
  if (diff < 1000) return '刚刚'
  if (diff < 60000) return Math.floor(diff / 1000) + ' 秒'
  if (diff < 3600000) return Math.floor(diff / 60000) + ' 分钟'
  if (diff < 86400000) return Math.floor(diff / 3600000) + ' 小时'
  return Math.floor(diff / 86400000) + ' 天'
}

export function time(ms) {
  const t = Number(ms) || 0
  if (!t) return '—'
  const d = new Date(t)
  const p = (n) => String(n).padStart(2, '0')
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
    ' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds())
}

// 磁盘用量：按总量选择 K/M/G，返回 {used,total,unit}
export function kbPair(usedKb, totalKb) {
  const k = Number(totalKb) || 0
  let unit = 'K'
  let div = 1
  if (k >= 100 * 1024) {
    unit = 'G'
    div = 1024 * 1024
  } else if (k >= 1024) {
    unit = 'M'
    div = 1024
  }
  const f = (v) => {
    const n = (Number(v) || 0) / div
    return Math.abs(n) >= 10 ? n.toFixed(1) : n.toFixed(2)
  }
  return { used: f(usedKb), total: f(totalKb), unit }
}

export function downloadBlob(blob, name) {
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')  // ui-allow: 浏览器下载要临时 <a>，Element 没有对应组件
  a.href = url
  a.download = name
  a.click()
  URL.revokeObjectURL(url)
}

export function downloadJson(name, data) {
  downloadBlob(new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' }), name)
}

export async function copyText(text, okMsg = '已复制') {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success(okMsg)
  } catch (e) {
    ElMessage.warning('复制失败，请手动选择')
  }
}
