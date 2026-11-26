// Markdown 渲染内核的自检（无浏览器依赖）。
// 用法：node scripts/check-markdown.mjs
//
// 重点不是“能不能渲染”，而是**不可信文本不能变成可执行标签**：
// 文档和模型回复都会进来，内联 HTML、javascript: 链接必须在渲染层就被挡掉。
import assert from 'node:assert'
import { escapeHtml, renderMarkdown } from '../src/shared/markdown.js'

let pass = 0
function check(name, fn) {
  try {
    fn()
    pass++
    console.log('  ok  ' + name)
  } catch (e) {
    console.error('FAIL  ' + name + '\n      ' + e.message)
    process.exitCode = 1
  }
}

const html = (src) => renderMarkdown(src).html

check('转义 HTML 特殊字符', () => {
  assert.equal(escapeHtml('<a href="x">&\'</a>'),
    '&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;')
})

check('内联 HTML 不会变成标签', () => {
  const out = html('<script>alert(1)</script>')
  assert.ok(!out.includes('<script'), out)
  assert.ok(out.includes('&lt;script&gt;'), out)
})

check('img onerror 被转义成文本', () => {
  const out = html('<img src=x onerror=alert(1)>')
  assert.ok(!/<img/i.test(out), out)
})

check('javascript: 链接降级为纯文本', () => {
  const out = html('[点我](javascript:alert(1))')
  assert.ok(!out.includes('href'), out)
  assert.ok(out.includes('点我'), out)
})

check('正常链接保留并加 noopener', () => {
  const out = html('[文档](https://example.com/a)')
  assert.ok(out.includes('href="https://example.com/a"'), out)
  assert.ok(out.includes('rel="noopener noreferrer"'), out)
})

check('站内路由链接不加 target', () => {
  const out = html('[权限组](#/groups)')
  assert.ok(out.includes('href="#/groups"'), out)
  assert.ok(!out.includes('target='), out)
})

check('标题生成锚点，支持 {#id}', () => {
  const r = renderMarkdown('## 一、部署\n\n### 细节 {#detail}\n')
  assert.deepEqual(r.headings.map((h) => [h.level, h.id]), [[2, '一-部署'], [3, 'detail']])
  assert.ok(r.html.includes('id="detail"'), r.html)
})

check('围栏代码带语言与复制按钮', () => {
  const out = html('```bash\necho $HOME\n```')
  assert.ok(out.includes('data-lang="bash"'), out)
  assert.ok(out.includes('class="md-copy"'), out)
  assert.ok(out.includes('echo $HOME'), out)
})

check('代码块里的 HTML 不被解释', () => {
  const out = html('```html\n<b>x</b>\n```')
  assert.ok(out.includes('&lt;b&gt;x&lt;/b&gt;'), out)
})

check('表格渲染与对齐', () => {
  const out = html('| 项 | 值 |\n|:---|--:|\n| a | 1 |\n| b | 2 |')
  assert.ok(out.includes('<th style="text-align:left">项</th>'), out)
  assert.ok(out.includes('<td style="text-align:right">1</td>'), out)
  assert.equal((out.match(/<tr>/g) || []).length, 3, out)
})

check('无序列表嵌套', () => {
  const out = html('- a\n  - b\n  - c\n- d')
  assert.ok(out.startsWith('<ul><li>a<ul><li>b</li><li>c</li></ul></li><li>d</li></ul>'), out)
})

check('有序列表与任务列表', () => {
  const out = html('1. 第一步\n2. 第二步\n\n- [x] 已完成\n- [ ] 待办')
  assert.ok(out.includes('<ol><li>第一步</li><li>第二步</li></ol>'), out)
  assert.ok(out.includes('md-check">☑'), out)
  assert.ok(out.includes('md-check">☐'), out)
})

check('引用与分隔线', () => {
  const out = html('> 注意 **风险**\n\n---\n')
  assert.ok(out.includes('<blockquote>注意 <strong>风险</strong></blockquote>'), out)
  assert.ok(out.includes('<hr>'), out)
})

check('裸 URL 自动链接', () => {
  const out = html('见 https://example.com/x 说明')
  assert.ok(out.includes('<a href="https://example.com/x"'), out)
})

check('行内代码里的 * 不当强调', () => {
  const out = html('用 `a*b*c` 表示')
  assert.ok(out.includes('<code>a*b*c</code>'), out)
  assert.ok(!out.includes('<em>'), out)
})

check('段落内换行保留为 br', () => {
  const out = html('第一行\n第二行\n\n新段落')
  assert.ok(out.includes('第一行<br>第二行'), out)
  assert.equal((out.match(/<p>/g) || []).length, 2, out)
})

console.log(pass + ' markdown checks passed')
