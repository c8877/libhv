// 校验 src/shared/api-docs.js 与 relay/admin/backend/admin_routes.cpp 是否对表。
// 用法：cd relay/admin/web && npm run check:docs
//
// 只比较 method + path 是否存在，不校验角色（角色在 handler 里还可能二次判断）。
// 忽略：静态资源、登录/初始化、API Token 自身管理（文档中以「不可用」说明）、/device/* 反代（文档合并为一条）。
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, resolve } from 'node:path'
import { DOCS } from '../src/shared/api-docs.js'

const here = dirname(fileURLToPath(import.meta.url))
const routesFile = resolve(here, '../../backend/admin_routes.cpp')
const src = readFileSync(routesFile, 'utf8')

const ignore = (method, path) =>
  path === '/' ||
  path.startsWith('/assets/') ||
  path.startsWith('/api/v1/setup') ||
  path === '/api/v1/login' ||
  path === '/api/v1/logout' ||
  path === '/api/v1/openapi.json' ||
  path.startsWith('/api/v1/admin-tokens') ||
  path.startsWith('/device/')

const key = (method, path) => method.toUpperCase() + ' ' + path

const routes = new Map()
// service_.Handle("GET", "/api/v1/xxx", ...)
for (const m of src.matchAll(/service_\.Handle\("([A-Z]+)",\s*"([^"]+)"/g)) {
  const [, method, path] = m
  if (!ignore(method, path)) routes.set(key(method, path), { method, path })
}
// access("<resource>", "GET", "/api/v1/xxx", ...)
for (const m of src.matchAll(/access\(\s*"[^"]*"\s*,\s*"([A-Z]+)"\s*,\s*"([^"]+)"/g)) {
  const [, method, path] = m
  if (!ignore(method, path)) routes.set(key(method, path), { method, path })
}
// 旧式 access(ROLE_X, "GET", "/api/v1/xxx", ...)
for (const m of src.matchAll(/access\(ROLE_[A-Z]+,\s*"([A-Z]+)",\s*"([^"]+)"/g)) {
  const [, method, path] = m
  if (!ignore(method, path)) routes.set(key(method, path), { method, path })
}
// 兜底：多行书写的 access
for (const m of src.matchAll(/access\(\s*\n\s*"[^"]*",\s*\n\s*"([A-Z]+)",\s*\n\s*"([^"]+)"/g)) {
  const [, method, path] = m
  if (!ignore(method, path)) routes.set(key(method, path), { method, path })
}
// 兜底：多行书写的 service_.Handle
for (const m of src.matchAll(/service_\.Handle\(\s*\n\s*"([A-Z]+)",\s*\n\s*"([^"]+)"/g)) {
  const [, method, path] = m
  if (!ignore(method, path)) routes.set(key(method, path), { method, path })
}

const documented = new Map()
for (const d of DOCS) {
  if (ignore(d.method, d.path)) continue
  // /device/* 在路由里按 6 个方法注册，文档合并成一条
  documented.set(key(d.method, d.path), d)
}

const missing = [...routes.values()].filter((r) => !documented.has(key(r.method, r.path)))
const stale = [...documented.values()].filter((d) => !routes.has(key(d.method, d.path)))
const deviceDoc = DOCS.some((d) => d.path.startsWith('/device/'))

if (missing.length || stale.length || !deviceDoc) {
  console.error('[api-docs] 与 admin_routes.cpp 不一致')
  if (missing.length) {
    console.error('\n文档缺少以下接口：')
    for (const r of missing) console.error(`  ${r.method} ${r.path}`)
  }
  if (stale.length) {
    console.error('\n文档里有、路由里没有：')
    for (const d of stale) console.error(`  ${d.method} ${d.path}`)
  }
  if (!deviceDoc) console.error('\n文档缺少 /device/{devid}/{port}/* 反代说明')
  process.exit(1)
}

console.log(`[api-docs] OK: 路由 ${routes.size} 条，文档 ${documented.size} 条（另有 /device/* 合并说明）`)
