// 校验管理台前端不要重复实现 Element Plus 已有的能力。
// 用法：cd relay/admin/web && npm run check:ui
//
// 规则见仓库根目录 AGENTS.md「前端（管理台）组件规则」。
// Element 确实没有对应能力时，用下面两种注释显式放行，并写清原因：
//   <!-- ui-allow: 原因 -->                     单行
//   <!-- ui-allow-begin: 原因 --> ... <!-- ui-allow-end -->   成块
// 脚本区放行用 // ui-allow: 原因（.vue 的模板规则和 .js 的 DOM 规则都会检查）
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join, relative, resolve } from 'node:path'

const here = dirname(fileURLToPath(import.meta.url))
const srcDir = resolve(here, '../src')

const rules = [
  {
    id: 'raw-form-control',
    scope: 'template',
    re: /<(input|select|textarea)\b/g,
    what: '原生表单控件',
    fix: '改用 el-input / el-select / el-date-picker / el-switch',
  },
  {
    id: 'raw-table',
    scope: 'template',
    re: /<table\b/g,
    what: '手写 table',
    fix: '改用 el-table；文档展示表用 ui-allow 标注',
  },
  {
    id: 'raw-button',
    scope: 'template',
    re: /<button\b/g,
    what: '原生 button',
    fix: '改用 el-button',
  },
  {
    id: 'pseudo-control',
    scope: 'template',
    re: /\brole="(button|tab|checkbox|radio|switch|menuitem|option)"/g,
    what: 'ARIA 伪控件',
    fix: '改用 el-button / el-tabs / el-checkbox / el-radio / el-switch',
  },
  {
    id: 'custom-overlay',
    scope: 'template',
    re: /class="[^"]*\b(modal|overlay|mask)\b[^"]*"/g,
    what: '自定义遮罩/弹层',
    fix: '改用 el-dialog / el-drawer / el-popover',
  },
  {
    id: 'manual-dom',
    scope: 'file',
    re: /\bdocument\.createElement\b|\.innerHTML\s*=|insertAdjacentHTML\b/g,
    what: '手写 DOM',
    fix: '改用模板和响应式状态；下载等浏览器能力用 ui-allow 标注',
  },
]

function walk(dir) {
  const out = []
  for (const name of readdirSync(dir)) {
    const p = join(dir, name)
    const st = statSync(p)
    if (st.isDirectory()) out.push(...walk(p))
    else if (name.endsWith('.vue') || name.endsWith('.js')) out.push(p)
  }
  return out
}

const violations = []

for (const file of walk(srcDir)) {
  const text = readFileSync(file, 'utf8')
  const lines = text.split('\n')
  const tplStart = text.indexOf('<template>')
  const tplEnd = text.lastIndexOf('</template>')
  const lineStarts = []
  let pos = 0
  for (const line of lines) {
    lineStarts.push(pos)
    pos += line.length + 1
  }
  const lineOf = (index) => {
    let lo = 0
    let hi = lineStarts.length - 1
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1
      if (lineStarts[mid] <= index) lo = mid
      else hi = mid - 1
    }
    return lo + 1
  }

  // 同一行，或紧挨着的上一行（模板里注释只能写在标签上面）
  const allowedLine = (n) =>
    (lines[n - 1] || '').includes('ui-allow:') || (lines[n - 2] || '').includes('ui-allow:')

  // 成块放行：ui-allow-begin 到 ui-allow-end 之间
  const blockAllow = new Array(lines.length + 1).fill(false)
  let depth = 0
  lines.forEach((line, i) => {
    if (line.includes('ui-allow-begin')) depth++
    blockAllow[i + 1] = depth > 0
    if (line.includes('ui-allow-end')) depth = Math.max(0, depth - 1)
  })

  for (const rule of rules) {
    if (rule.scope === 'template' && (tplStart < 0 || tplEnd < 0)) continue
    for (const m of text.matchAll(rule.re)) {
      if (rule.scope === 'template' && (m.index < tplStart || m.index > tplEnd)) continue
      const line = lineOf(m.index)
      if (allowedLine(line) || blockAllow[line]) continue
      violations.push({
        file: relative(srcDir, file),
        line,
        rule: rule.id,
        what: rule.what,
        fix: rule.fix,
        snippet: (lines[line - 1] || '').trim().slice(0, 100),
      })
    }
  }
}

if (violations.length) {
  console.error(`[ui-rules] 发现 ${violations.length} 处重复实现 Element 已有能力：`)
  for (const v of violations) {
    console.error(`\n  ${v.file}:${v.line}  ${v.what}`)
    console.error(`    ${v.snippet}`)
    console.error(`    -> ${v.fix}`)
  }
  console.error('\n规则见 AGENTS.md「前端（管理台）组件规则」。确认 Element 无对应能力时用 ui-allow 注释放行。')
  process.exit(1)
}

console.log('[ui-rules] OK: 管理台未发现重复实现 Element 能力的代码')
