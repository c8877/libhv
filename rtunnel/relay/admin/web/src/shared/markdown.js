// Markdown 渲染内核（无第三方依赖）。
//
// 安全模型：**先转义、再按白名单拼标签**。文档与模型回复都当成不可信输入，
// 内联 HTML（<script>、<img onerror>）一律转成文本；链接只允许 http(s)、mailto
// 和站内锚点，`javascript:` 之类直接降级成纯文本。
//
// 支持范围（够写运维文档与对话回复，不做完整 CommonMark）：
//   标题 h1-h4（支持 `## 标题 {#anchor}` 指定锚点）
//   段落、换行、分隔线
//   无序/有序列表（2 空格缩进嵌套）、任务列表 `- [x]`
//   表格（含 :---: 对齐）、引用 `>`
//   围栏代码 ```lang（带语言标签与复制按钮）
//   行内 code / **粗体** / *斜体* / ~~删除线~~ / 链接 / 图片 / 裸 URL 自动链接

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }

export function escapeHtml(text) {
  return String(text == null ? '' : text).replace(/[&<>"']/g, (c) => ESCAPES[c])
}

// 只接受显式协议或站内锚点；其余（javascript:、data:、vbscript:）返回空串
function safeUrl(url) {
  const u = String(url || '').trim()
  if (!u) return ''
  if (/^(https?:\/\/|mailto:)/i.test(u)) return u
  if (u.startsWith('#') || u.startsWith('/')) return u
  return ''
}

function inline(text) {
  // 行内代码先摘出来，避免里面的 * _ ~ 被当成强调
  const codes = []
  let s = String(text == null ? '' : text).replace(/`([^`]+)`/g, (m, code) => {
    codes.push(escapeHtml(code))
    return '\u0000' + (codes.length - 1) + '\u0000'
  })
  s = escapeHtml(s)

  s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (m, alt, url) => {
    const u = safeUrl(url)
    return u ? `<img src="${u}" alt="${alt}" loading="lazy">` : alt
  })
  s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (m, label, url) => {
    const u = safeUrl(url)
    if (!u) return label
    const ext = /^https?:/i.test(u)
    return `<a href="${u}"${ext ? ' target="_blank" rel="noopener noreferrer"' : ''}>${label}</a>`
  })
  // 裸 URL：前面必须是行首、空白或左括号，避免把已有 <a href> 再包一层
  s = s.replace(/(^|[\s(（])(https?:\/\/[^\s<)）]+)/g,
    (m, pre, url) => `${pre}<a href="${url}" target="_blank" rel="noopener noreferrer">${url}</a>`)

  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
  s = s.replace(/(^|[^*\w])\*([^*\n]+)\*/g, '$1<em>$2</em>')
  s = s.replace(/~~([^~]+)~~/g, '<del>$1</del>')

  return s.replace(/\u0000(\d+)\u0000/g, (m, i) => '<code>' + codes[Number(i)] + '</code>')
}

// 中文没有词边界，只保留字母/数字/汉字，其余折成 -
function slugify(text, used) {
  const base = String(text)
    .replace(/<[^>]+>/g, '')
    .replace(/[*`_~]/g, '')
    .toLowerCase()
    .replace(/[^\p{L}\p{N}]+/gu, '-')
    .replace(/^-+|-+$/g, '') || 'sec'
  let id = base
  let n = 1
  while (used.has(id)) id = base + '-' + (++n)
  used.add(id)
  return id
}

function codeBlock(code, lang) {
  const label = lang || 'text'
  return `<div class="md-code" data-lang="${escapeHtml(lang)}">` +
    `<div class="md-code-bar"><span>${escapeHtml(label)}</span>` +
    `<button type="button" class="md-copy">复制</button></div>` +
    `<pre><code>${escapeHtml(code)}</code></pre></div>`
}

function parseTableRow(line) {
  let s = line.trim()
  if (s.startsWith('|')) s = s.slice(1)
  if (s.endsWith('|')) s = s.slice(0, -1)
  // 支持单元格里的 \| 转义
  return s.split(/(?<!\\)\|/).map((c) => c.trim().replace(/\\\|/g, '|'))
}

function tableHtml(head, aligns, rows) {
  const cell = (tag, text, idx) => {
    const a = aligns[idx]
    const style = a ? ` style="text-align:${a}"` : ''
    return `<${tag}${style}>${inline(text)}</${tag}>`
  }
  const thead = '<thead><tr>' + head.map((c, i) => cell('th', c, i)).join('') + '</tr></thead>'
  const tbody = rows.length
    ? '<tbody>' + rows.map((r) =>
      '<tr>' + head.map((_, i) => cell('td', r[i] == null ? '' : r[i], i)).join('') + '</tr>').join('') + '</tbody>'
    : ''
  return `<div class="md-table"><table>${thead}${tbody}</table></div>`
}

function buildList(items) {
  let html = ''
  const stack = []
  for (const it of items) {
    while (stack.length && it.indent < stack[stack.length - 1].indent) {
      const top = stack.pop()
      html += `</li></${top.tag}>`
    }
    const top = stack[stack.length - 1]
    if (!top || it.indent > top.indent) {
      const tag = it.ordered ? 'ol' : 'ul'
      html += `<${tag}>`
      stack.push({ indent: it.indent, tag })
    } else {
      html += '</li>'
    }
    const task = /^\[([ xX])\]\s+([\s\S]*)$/.exec(it.text)
    if (task) {
      const mark = task[1].toLowerCase() === 'x' ? '☑' : '☐'
      html += `<li class="md-task"><span class="md-check">${mark}</span> ${inline(task[2])}`
    } else {
      html += `<li>${inline(it.text)}`
    }
  }
  while (stack.length) {
    const top = stack.pop()
    html += `</li></${top.tag}>`
  }
  return html
}

/**
 * 渲染 markdown。
 * @param {string} src
 * @returns {{html: string, headings: Array<{level:number,id:string,text:string}>}}
 */
export function renderMarkdown(src) {
  const lines = String(src == null ? '' : src).replace(/\r\n?/g, '\n').split('\n')
  const blocks = []
  const headings = []
  const used = new Set()

  let para = []
  const flushPara = () => {
    if (!para.length) return
    blocks.push('<p>' + para.map((l) => inline(l)).join('<br>') + '</p>')
    para = []
  }

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i]
    const line = raw.replace(/\s+$/, '')

    // 围栏代码
    const fence = /^\s*```(\S*)\s*$/.exec(line)
    if (fence) {
      flushPara()
      const lang = fence[1] || ''
      const buf = []
      i++
      while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) buf.push(lines[i++])
      blocks.push(codeBlock(buf.join('\n'), lang))
      continue
    }

    if (!line.trim()) {
      flushPara()
      continue
    }

    // 标题
    const hm = /^(#{1,4})\s+(.+?)\s*$/.exec(line)
    if (hm) {
      flushPara()
      const level = hm[1].length
      let text = hm[2]
      let id = ''
      const am = /\s*\{#([\w-]+)\}\s*$/.exec(text)
      if (am) {
        id = am[1]
        text = text.slice(0, am.index)
        used.add(id)
      } else {
        id = slugify(text, used)
      }
      const plain = text.replace(/[*`_~]/g, '')
      headings.push({ level, id, text: plain })
      blocks.push(`<h${level} id="${escapeHtml(id)}">${inline(text)}</h${level}>`)
      continue
    }

    // 分隔线（要放在列表前面：--- 不是列表项）
    if (/^\s*(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) {
      flushPara()
      blocks.push('<hr>')
      continue
    }

    // 表格：本行有 | 且下一行是分隔行
    if (line.includes('|') && i + 1 < lines.length &&
        /^\s*\|?[\s:|-]+\|?\s*$/.test(lines[i + 1]) && lines[i + 1].includes('-')) {
      flushPara()
      const head = parseTableRow(line)
      const aligns = parseTableRow(lines[i + 1]).map((c) => {
        const l = c.startsWith(':')
        const r = c.endsWith(':')
        if (l && r) return 'center'
        if (r) return 'right'
        if (l) return 'left'
        return ''
      })
      i += 2
      const rows = []
      while (i < lines.length && lines[i].includes('|') && lines[i].trim()) {
        rows.push(parseTableRow(lines[i]))
        i++
      }
      i--
      blocks.push(tableHtml(head, aligns, rows))
      continue
    }

    // 引用
    if (/^\s*>\s?/.test(line)) {
      flushPara()
      const buf = []
      while (i < lines.length && /^\s*>\s?/.test(lines[i])) {
        buf.push(lines[i].replace(/^\s*>\s?/, ''))
        i++
      }
      i--
      blocks.push('<blockquote>' + buf.map((l) => inline(l)).join('<br>') + '</blockquote>')
      continue
    }

    // 列表：一次收完连续的列表项（含嵌套）
    const lm = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(raw)
    if (lm) {
      flushPara()
      const items = []
      while (i < lines.length) {
        const m = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(lines[i])
        if (!m) break
        items.push({
          indent: m[1].replace(/\t/g, '  ').length,
          ordered: /^\d/.test(m[2]),
          text: m[3],
        })
        i++
      }
      i--
      blocks.push(buildList(items))
      continue
    }

    para.push(line)
  }
  flushPara()

  return { html: blocks.join('\n'), headings }
}

// 提取标题（不生成 HTML），给只需要目录的地方用
export function markdownHeadings(src) {
  return renderMarkdown(src).headings
}
