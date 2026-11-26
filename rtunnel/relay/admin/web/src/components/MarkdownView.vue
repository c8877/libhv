<!--
  文档展示组件：把 Markdown 渲染成管理台样式。

  两种用法：
    <MarkdownView :source="text" />              直接渲染一段 markdown 文本（对话回复等）
    <MarkdownView src="/docs/guide.md" toc />    拉取并渲染一个 .md 文件（同源，带 Cookie）

  Element Plus 没有 markdown 渲染能力，这里必须用 v-html 输出；
  HTML 由 shared/markdown.js 生成——所有文本先转义、标签按白名单拼，链接只允许
  http(s)/mailto/站内，内联 HTML 一律当文本，因此不存在脚本注入面。
-->
<template>
  <div class="md-wrap" :class="{ dense }">
    <el-skeleton v-if="loading" :rows="6" animated />
    <el-alert v-else-if="error" :title="error" type="error" :closable="false" show-icon />
    <!-- ui-allow: Element 无 markdown 组件；内容由 shared/markdown.js 转义后生成白名单标签 -->
    <div v-else class="md-body" v-html="html" @click="onClick" />
    <div v-if="!loading && !error && !html" class="md-empty muted">{{ emptyText }}</div>
  </div>
</template>

<script setup>
import { onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { renderMarkdown } from '@/shared/markdown.js'

const props = defineProps({
  source: { type: String, default: '' },
  src: { type: String, default: '' },
  dense: { type: Boolean, default: false },
  emptyText: { type: String, default: '这里还没有内容' },
})

const emit = defineEmits(['rendered'])

const router = useRouter()
const html = ref('')
const loading = ref(false)
const error = ref('')

// 同一份文档可能被多个页面打开：按 url 缓存，避免来回切换时重复拉
const cache = new Map()

async function loadFile(url) {
  if (cache.has(url)) return cache.get(url)
  const p = fetch(url, { credentials: 'same-origin' }).then(async (r) => {
    if (!r.ok) throw new Error('加载失败：HTTP ' + r.status)
    return r.text()
  })
  cache.set(url, p)
  try {
    return await p
  } catch (e) {
    cache.delete(url)   // 失败不缓存，下次还能重试
    throw e
  }
}

async function render(src) {
  if (!src) {
    html.value = ''
    return
  }
  const { html: out, headings } = renderMarkdown(src)
  html.value = out
  emit('rendered', headings)
}

async function load() {
  error.value = ''
  if (props.src) {
    loading.value = true
    try {
      await render(await loadFile(props.src))
    } catch (e) {
      error.value = e.message || '加载失败'
    } finally {
      loading.value = false
    }
    return
  }
  render(props.source)
}

async function copyCode(btn) {
  const box = btn.closest('.md-code')
  const code = box && box.querySelector('code')
  if (!code) return
  const text = code.innerText
  try {
    await navigator.clipboard.writeText(text)
  } catch (e) {
    // 非 HTTPS 下没有 clipboard API：选中再复制，不创建临时节点
    const range = document.createRange()
    range.selectNodeContents(code)
    const sel = window.getSelection()
    sel.removeAllRanges()
    sel.addRange(range)
    const ok = document.execCommand && document.execCommand('copy')
    sel.removeAllRanges()
    if (!ok) {
      ElMessage.warning('复制失败，请手动选中')
      return
    }
  }
  ElMessage.success('已复制')
  btn.textContent = '已复制'
  setTimeout(() => { btn.textContent = '复制' }, 1200)
}

function onClick(e) {
  const copyBtn = e.target.closest('.md-copy')
  if (copyBtn) {
    e.preventDefault()
    copyCode(copyBtn)
    return
  }
  const a = e.target.closest('a')
  if (!a) return
  const href = a.getAttribute('href') || ''
  // 站内路由：文档里写 [权限组](#/groups) 就能跳页面
  if (href.startsWith('#/')) {
    e.preventDefault()
    router.push(href.slice(1))
    return
  }
  if (href.startsWith('#')) {
    e.preventDefault()
    const el = document.getElementById(href.slice(1))
    if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' })
  }
}

watch(() => [props.source, props.src], load)
onMounted(load)

defineExpose({ html })
</script>

<style scoped>
.md-wrap { font-size: 13.5px; line-height: 1.8; }
.md-wrap.dense { font-size: 13px; line-height: 1.7; }

.md-body :deep(h1),
.md-body :deep(h2),
.md-body :deep(h3),
.md-body :deep(h4) {
  margin: 18px 0 8px;
  font-weight: 600;
  line-height: 1.4;
  scroll-margin-top: 12px;
}
.md-body :deep(h1) { font-size: 20px; }
.md-body :deep(h2) {
  font-size: 17px;
  padding-bottom: 6px;
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.md-body :deep(h3) { font-size: 15px; }
.md-body :deep(h4) { font-size: 14px; color: var(--el-text-color-regular); }
.md-body :deep(h1:first-child),
.md-body :deep(h2:first-child),
.md-body :deep(h3:first-child) { margin-top: 0; }

.md-body :deep(p) { margin: 0 0 10px; }
.md-body :deep(p:last-child) { margin-bottom: 0; }
.md-body :deep(strong) { font-weight: 600; }
.md-body :deep(hr) {
  border: 0;
  border-top: 1px solid var(--el-border-color-lighter);
  margin: 16px 0;
}

.md-body :deep(ul),
.md-body :deep(ol) { margin: 6px 0 10px; padding-left: 22px; }
.md-body :deep(li) { margin: 2px 0; }
.md-body :deep(li > ul),
.md-body :deep(li > ol) { margin: 2px 0; }
.md-body :deep(.md-task) { list-style: none; margin-left: -18px; }
.md-body :deep(.md-check) { color: var(--el-color-primary); margin-right: 2px; }

.md-body :deep(blockquote) {
  margin: 10px 0;
  padding: 8px 12px;
  border-left: 3px solid var(--el-color-primary-light-5);
  background: var(--el-fill-color-light);
  color: var(--el-text-color-regular);
  border-radius: 0 4px 4px 0;
}
.md-body :deep(blockquote p) { margin: 0; }

.md-body :deep(a) { color: var(--el-color-primary); text-decoration: none; }
.md-body :deep(a:hover) { text-decoration: underline; }
.md-body :deep(img) { max-width: 100%; border-radius: 4px; }

.md-body :deep(code) {
  font-family: var(--el-font-family-mono, ui-monospace, SFMono-Regular, Menlo, monospace);
  font-size: 12.5px;
  background: var(--el-fill-color);
  padding: 1px 5px;
  border-radius: 3px;
}

.md-body :deep(.md-code) {
  margin: 10px 0;
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 6px;
  overflow: hidden;
  background: var(--el-fill-color-lighter);
}
.md-body :deep(.md-code-bar) {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 4px 10px;
  font-size: 11.5px;
  color: var(--el-text-color-secondary);
  background: var(--el-fill-color);
  border-bottom: 1px solid var(--el-border-color-lighter);
  user-select: none;
}
.md-body :deep(.md-copy) {
  border: 0;
  background: transparent;
  color: var(--el-color-primary);
  font-size: 11.5px;
  cursor: pointer;
  padding: 0 2px;
}
.md-body :deep(.md-code pre) {
  margin: 0;
  padding: 10px 12px;
  overflow-x: auto;
}
.md-body :deep(.md-code code) {
  background: transparent;
  padding: 0;
  font-size: 12.5px;
  line-height: 1.6;
  white-space: pre;
}

.md-body :deep(.md-table) { margin: 10px 0; overflow-x: auto; }
.md-body :deep(table) {
  border-collapse: collapse;
  width: 100%;
  font-size: 13px;
}
.md-body :deep(th),
.md-body :deep(td) {
  border: 1px solid var(--el-border-color-lighter);
  padding: 6px 10px;
  text-align: left;
  vertical-align: top;
}
.md-body :deep(th) {
  background: var(--el-fill-color-light);
  font-weight: 600;
  white-space: nowrap;
}

.md-empty { padding: 16px 0; text-align: center; }
</style>
