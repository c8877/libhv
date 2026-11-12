<template>
  <div class="panel term-panel">
    <div class="panel-head">
      <h2>终端</h2>
      <span class="muted">{{ activeDevid }}</span>
      <span class="spacer" />
      <el-button @click="newTerm">新开</el-button>
      <el-button :disabled="!active" @click="closeCurrent">关闭当前</el-button>
      <el-button @click="$router.push('/agents')">返回设备</el-button>
    </div>

    <div v-if="tabs.length > 1" class="term-tabs">
      <button v-for="s in tabs" :key="s.id" :class="{ on: s.id === active }" @click="activate(s.id)">
        {{ s.devid }}<span v-if="!s.alive" class="dead"> ✕</span>
      </button>
    </div>

    <div ref="host" class="term-host" />
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import '@xterm/xterm/css/xterm.css'
import { TermMgr } from '~/shared/term.js'
import { auth } from '~/shared/store.js'

const route = useRoute()
const router = useRouter()
const host = ref(null)
const active = ref('')
const tabs = ref([])

const activeDevid = computed(() => {
  const s = TermMgr.get(active.value)
  return s ? s.devid : ''
})

function sync() {
  tabs.value = TermMgr.list().map((s) => ({
    id: s.id, devid: s.devid, alive: s.alive,
  }))
  if (!TermMgr.get(active.value)) active.value = tabs.value.length ? tabs.value[tabs.value.length - 1].id : ''
}

function attach() {
  const s = TermMgr.get(active.value)
  if (!s || !host.value) return
  if (!s.opened) {
    s.term.open(s.box)
    s.opened = true
  }
  host.value.replaceChildren(s.box)
  nextTick(() => {
    try { s.fit.fit() } catch (e) { /* 容器尚未布局 */ }
  })
}

function activate(id) {
  active.value = id
  attach()
}

function ensure(devid) {
  let s = TermMgr.aliveOf(devid)
  if (!s) s = TermMgr.open(devid, Number(auth.profile.term_font) || 14)
  active.value = s.id
  sync()
  nextTick(attach)
}

async function newTerm() {
  let devid
  try {
    const cur = TermMgr.get(active.value)
    const r = await ElMessageBox.prompt('设备 devid，或 a:用户名', '新建终端', {
      inputValue: cur ? cur.devid : '',
      confirmButtonText: '打开',
      cancelButtonText: '取消',
      inputValidator: (v) => (String(v || '').trim() ? true : '请输入 devid'),
    })
    devid = String(r.value || '').trim()
  } catch (e) { return }
  if (!devid) return
  ensure(devid)
}

function closeCurrent() {
  if (!active.value) return
  TermMgr.close(active.value)
  sync()
  if (!tabs.value.length) {
    ElMessage.info('没有打开的终端')
    router.replace('/agents')
    return
  }
  attach()
}

let onResize = null

onMounted(() => {
  TermMgr.onChange = sync
  const devid = decodeURIComponent(route.params.devid || '')
  if (!devid) {
    router.replace('/agents')
    return
  }
  ensure(devid)
  onResize = () => {
    const s = TermMgr.get(active.value)
    if (s) { try { s.fit.fit() } catch (e) { /* 忽略 */ } }
  }
  window.addEventListener('resize', onResize)
})

watch(() => route.params.devid, (v) => {
  if (!v) return
  const devid = decodeURIComponent(String(v))
  const cur = TermMgr.get(active.value)
  if (cur && cur.devid === devid) return
  ensure(devid)
})

onBeforeUnmount(() => {
  TermMgr.onChange = null
  if (onResize) { window.removeEventListener('resize', onResize); onResize = null }
  const s = TermMgr.get(active.value)
  if (s && s.box.parentNode) s.box.parentNode.removeChild(s.box)
})
</script>

<style scoped>
.term-panel {
  display: flex;
  flex-direction: column;
  height: calc(100vh - 96px);
  margin-bottom: 0;
}
.term-tabs { display: flex; gap: 6px; margin-bottom: 8px; flex-wrap: wrap; }
.term-tabs button {
  border: 1px solid var(--el-border-color);
  background: var(--el-fill-color-blank);
  color: var(--el-text-color-regular);
  border-radius: 6px;
  padding: 4px 10px;
  font-size: 12px;
  cursor: pointer;
}
.term-tabs button.on {
  border-color: var(--el-color-primary);
  color: var(--el-color-primary);
  background: var(--el-color-primary-light-9);
}
.term-tabs .dead { color: var(--el-color-danger); }
.term-host {
  flex: 1;
  min-height: 0;
  padding: 6px;
  border-radius: 6px;
  background: #1e1e1e;
  overflow: hidden;
}
:deep(.term-box) { width: 100%; height: 100%; }
@media (max-width: 959px) {
  .term-panel { height: calc(100vh - 88px); }
}
@media (max-width: 599px) {
  .term-panel { height: calc(100vh - 76px); }
  .panel-head { flex-wrap: wrap; gap: 8px; }
}
</style>
