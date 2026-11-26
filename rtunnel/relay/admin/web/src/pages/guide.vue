<template>
  <div class="guide">
    <div class="panel">
      <div class="panel-head">
        <h2>使用指南</h2>
        <span class="spacer" />
        <el-button text @click="$router.push('/docs')">接口文档</el-button>
      </div>
      <div class="toc">
        <a v-for="h in toc" :key="h.id" class="toc-item" @click="go(h.id)">{{ h.text }}</a>
      </div>
    </div>

    <div class="panel">
      <MarkdownView :source="doc" @rendered="onRendered" />
    </div>
  </div>
</template>

<script setup>
// 内容维护在 src/docs/guide.md：改文档不用动组件，渲染由 MarkdownView 统一负责
import { ref } from 'vue'
import MarkdownView from '@/components/MarkdownView.vue'
import doc from '@/docs/guide.md?raw'

const toc = ref([])

// 二级标题就是章节；目录与正文同源，不会出现“加了章节忘了目录”
function onRendered(headings) {
  toc.value = (headings || []).filter((h) => h.level === 2)
}

function go(id) {
  const el = document.getElementById(id)
  if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' })
}
</script>

<style scoped>
.guide { max-width: 1000px; }
.toc { display: flex; flex-wrap: wrap; gap: 8px; padding-top: 2px; }
.toc-item {
  font-size: 12.5px;
  padding: 5px 10px;
  border: 1px solid var(--el-border-color);
  border-radius: 12px;
  cursor: pointer;
}
.toc-item:hover { border-color: var(--el-color-primary); color: var(--el-color-primary); }
</style>
