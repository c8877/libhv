// 把 Vite 的设备本地页产物内联成单个 HTML，写到 agent/web/index.html。
// agent 通过 CMake 把该文件 gzip 后嵌入二进制，离线可用，不依赖任何外部静态资源。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const webRoot = path.resolve(here, '..')
const dist = path.resolve(webRoot, '../../../agent/web/local-dist')
const outFile = path.resolve(webRoot, '../../../agent/web/index.html')

function readDist(ref) {
  const clean = String(ref).replace(/^\.?\//, '')
  const p = path.resolve(dist, clean)
  if (!p.startsWith(dist + path.sep)) throw new Error('非法资源路径: ' + ref)
  return fs.readFileSync(p, 'utf8')
}

const entry = ['index.html', 'local.html'].map((f) => path.join(dist, f)).find((f) => fs.existsSync(f))
if (!entry) throw new Error('找不到本地页入口 HTML: ' + dist)
let html = fs.readFileSync(entry, 'utf8')

// modulepreload 对内联后的单文件没有意义
html = html.replace(/\s*<link[^>]+rel="modulepreload"[^>]*>/g, '')

// 内联样式
html = html.replace(/<link[^>]+rel="stylesheet"[^>]+href="([^"]+)"[^>]*>/g, (m, href) => {
  const css = readDist(href)
  return '<style>\n' + css.replace(/<\/style/gi, '<\\/style') + '\n</style>'
})

// 内联入口脚本（单入口 + inlineDynamicImports，不会有额外 chunk）
html = html.replace(/<script[^>]+src="([^"]+)"[^>]*><\/script>/g, (m, src) => {
  const js = readDist(src)
  return '<script type="module">\n' + js.replace(/<\/script/gi, '<\\/script') + '\n</script>'
})

const banner = '<!-- 构建产物：源码在 rtunnel/relay/admin/web/src/components/DeviceView.vue，' +
  '执行 npm run build:local 生成，不要手改 -->\n'
if (!html.includes('构建产物：源码在')) html = html.replace('<!doctype html>', '<!doctype html>\n' + banner)

fs.writeFileSync(outFile, html)
const size = Buffer.byteLength(html)
console.log('wrote ' + path.relative(process.cwd(), outFile) + ' (' + (size / 1024).toFixed(1) + ' KB)')
