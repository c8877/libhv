// Element 表格统一排序方向：数值列降序优先，文本列升序优先
export const TEXT_ORDERS = ['ascending', 'descending']
export const NUM_ORDERS = ['descending', 'ascending']

export function numBy(fn) {
  return (row) => {
    const n = Number(fn(row))
    return Number.isFinite(n) ? n : 0
  }
}

// 先按全量数据排序再分页时用：order 为 Element 的 ascending / descending
export function sortByOrder(rows, prop, order, getters = {}) {
  if (!prop || !order) return rows
  const get = getters[prop] || ((r) => r[prop])
  const mul = order === 'descending' ? -1 : 1
  return rows.slice().sort((a, b) => {
    const x = get(a)
    const y = get(b)
    if (typeof x === 'number' && typeof y === 'number') return (x - y) * mul
    return String(x ?? '').localeCompare(String(y ?? ''), 'zh-CN') * mul
  })
}
