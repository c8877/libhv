// 管理接口封装：统一 JSON、错误消息与 401 跳登录
let onUnauthorized = null

export function setUnauthorizedHandler(fn) {
  onUnauthorized = fn
}

async function req(path, opt = {}) {
  const headers = Object.assign({ 'X-Requested-With': 'fetch' }, opt.headers || {})
  const r = await fetch(path, Object.assign({ cache: 'no-store' }, opt, { headers }))
  if (r.status === 401) {
    if (onUnauthorized) onUnauthorized()
    const err = new Error('未登录或会话已过期')
    err.status = 401
    throw err
  }
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

export const Api = {
  get: (p) => req(p),
  post: (p, body) => req(p, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {}),
  }),
  put: (p, body) => req(p, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {}),
  }),
  del: (p) => req(p, { method: 'DELETE' }),
}
