// 从 src/shared/api-docs.js 生成 OpenAPI 3.1，供 MCP / 其它工具消费。
//
// 为什么从这里生成：api-docs.js 已经被 scripts/check-api-docs.mjs 强制与
// admin_routes.cpp 对表，是「唯一数据源」。再手写一份 OpenAPI 只会慢慢不同步。
//
// 用法：
//   node scripts/gen-openapi.mjs                    # 写 dist/openapi.json
//   node scripts/gen-openapi.mjs --out /tmp/o.json
import { mkdirSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, resolve } from 'node:path'
import { DOCS, GROUPS } from '../src/shared/api-docs.js'

const here = dirname(fileURLToPath(import.meta.url))
const argv = process.argv.slice(2)
const argOf = (name, def) => {
  const i = argv.indexOf('--' + name)
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def
}

const out = resolve(argOf('out', resolve(here, '../dist/openapi.json')))
const version = process.env.RTUNNEL_VERSION || 'dev'

// /api/v1/agents/:devid → /api/v1/agents/{devid}
const toOpenApiPath = (p) => p.replace(/:([A-Za-z_][A-Za-z0-9_]*)/g, '{$1}')

// 稳定、可读的 operationId：GET /api/v1/agents/:devid/procs/:pid → get_agents_devid_procs_pid
function operationId(method, path) {
  const slug = path
    .replace(/^\/api\/v1\//, '')
    .replace(/^\/device\//, 'device_')
    .replace(/[:{}*]/g, '')
    .replace(/[^A-Za-z0-9]+/g, '_')
    .replace(/_+$/, '')
    .toLowerCase()
  return (method.toLowerCase() + '_' + slug).replace(/_+/g, '_')
}

const typeOfParam = (p) => (p.in === 'body' || p.in === 'header' ? 'string' : 'string')

// 尝试把示例当 JSON 解析，解析不了就按字符串给
function exampleOf(text) {
  if (typeof text !== 'string') return undefined
  const t = text.trim()
  if (!t) return undefined
  try {
    return JSON.parse(t)
  } catch {
    return t
  }
}

// docs 里有些条目只给了 curl 示例：从 -d '...' 里把请求体捞出来，模型照着填
function bodyFromCurl(curl) {
  if (typeof curl !== 'string') return undefined
  const m = /-d\s+'([^']+)'/.exec(curl) || /-d\s+"([^"]+)"/.exec(curl)
  return m ? exampleOf(m[1]) : undefined
}

function schemaOfParam(p) {
  const s = { type: typeOfParam(p) }
  if (p.desc) s.description = p.desc
  return s
}

const paths = {}
const skipped = []

for (const d of DOCS) {
  const method = String(d.method || '').toLowerCase()
  if (!method) {
    skipped.push(d.path)
    continue
  }
  const path = toOpenApiPath(d.path)
  const params = Array.isArray(d.params) ? d.params : []
  const pathParams = params.filter((p) => p.in === 'path')
  const queryParams = params.filter((p) => p.in === 'query')
  const headerParams = params.filter((p) => p.in === 'header')
  const bodyParams = params.filter((p) => p.in === 'body')

  const op = {
    operationId: operationId(d.method, d.path),
    summary: d.summary || d.path,
    tags: [d.group || 'other'],
    // 「读写」标记是 MCP 决定要不要暴露这个工具的依据
    'x-rtunnel-read-only': method === 'get',
    'x-rtunnel-group': d.group || 'other',
  }
  if (d.note) {
    // 额外知识（前置条件、错误语义、计费口径等）对模型非常有用，放进 description
    op.description = d.note
  }
  if (d.minRole) op['x-rtunnel-min-role'] = d.minRole

  const parameters = []
  for (const p of [...pathParams, ...queryParams, ...headerParams]) {
    parameters.push({
      name: p.name,
      in: p.in,
      required: p.in === 'path' ? true : Boolean(p.required),
      schema: schemaOfParam(p),
    })
  }
  if (parameters.length) op.parameters = parameters

  if (bodyParams.length) {
    const props = {}
    for (const p of bodyParams) props[p.name] = schemaOfParam(p)
    op.requestBody = {
      required: true,
      content: { 'application/json': { schema: { type: 'object', properties: props } } },
    }
  } else if (method !== 'get' && method !== 'head') {
    // 写操作即使文档没写字段表，也要有请求体占位：否则 MCP 工具根本发不出 body
    const example = exampleOf(d.body) || bodyFromCurl(d.curl)
    op.requestBody = {
      required: true,
      content: {
        'application/json': {
          schema: { type: 'object', additionalProperties: true },
          ...(example !== undefined ? { example } : {}),
        },
      },
    }
  }

  const resp = exampleOf(d.resp)
  op.responses = {
    '200': {
      description: '成功',
      content: { 'application/json': { schema: {}, example: resp } },
    },
    '401': { description: '未认证' },
    '403': { description: '无权限或设备不在授权范围' },
  }

  if (!paths[path]) paths[path] = {}
  paths[path][method] = op
}

const doc = {
  openapi: '3.1.0',
  info: {
    title: 'rtunnel admin API',
    version,
    description:
      '由 relay/admin/web/src/shared/api-docs.js 生成（该文件由 check:docs 强制与 admin_routes.cpp 对表）。\n' +
      '鉴权：Authorization: Bearer <atk- 令牌>，或管理台登录后的 rt_sid Cookie。\n' +
      '设备类接口都受权限组的设备范围限制；写操作建议走 operations 的 plan→apply→verify。',
  },
  servers: [{ url: argOf('server', '$RT') }],
  tags: GROUPS.map((g) => ({ name: g.key, description: g.label })),
  paths,
}

mkdirSync(dirname(out), { recursive: true })
writeFileSync(out, JSON.stringify(doc, null, 1) + '\n')

const ops = Object.values(paths).reduce((n, m) => n + Object.keys(m).length, 0)
const readOnly = Object.values(paths).reduce(
  (n, m) => n + Object.values(m).filter((o) => o['x-rtunnel-read-only']).length, 0)
console.log(`[openapi] ${out}: ${ops} 个操作（只读 ${readOnly}）`)
if (skipped.length) console.warn('[openapi] 跳过无 method 的条目:', skipped.join(', '))
