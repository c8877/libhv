export function bytes(n) {
  n = Number(n) || 0
  const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
  let i = 0
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024
    i++
  }
  const digits = i === 0 ? 0 : n >= 100 ? 0 : n >= 10 ? 1 : 2
  return n.toFixed(digits) + ' ' + units[i]
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

export function downloadJson(name, data) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = name
  a.click()
  URL.revokeObjectURL(a.href)
}
