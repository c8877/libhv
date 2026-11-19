<template>
  <div :class="embedded ? 'tab-body' : 'panel'">
    <div :class="embedded ? 'tab-toolbar' : 'panel-head'">
      <h2 v-if="!embedded">文件管理</h2>
      <el-tag v-if="sys && fsOn" :type="writable ? 'success' : 'info'" size="small">{{ writable ? '读写' : '只读' }}</el-tag>
      <span class="spacer" />
      <el-button type="primary" size="small" :disabled="!sys || !fsOn" @click="toggle">{{ expanded ? '收起文件管理' : '打开文件管理' }}</el-button>
    </div>
    <div class="fs-card">
      <div class="mono fs-line">
        <template v-if="!sys">正在读取设备能力…</template>
        <template v-else-if="fsOn">{{ root }} · 单文件上限 {{ maxMb }} MB<span v-if="usage"> · {{ usage }}</span><span v-if="!writable"> · 当前角色只能浏览和下载</span></template>
        <template v-else>该设备未启用文件服务（可在「能力配置」中开启）</template>
      </div>
    </div>
    <div ref="fsHost" class="fs-host"></div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'
import { bytes } from '@/shared/utils.js'
import editorUrl from '../../../../../shared/web/cm-editor.bundle.js?url'
import '../../../../../shared/web/fs-ui.js'
import '../../../../../shared/web/file-view.js'

const RtunnelFs = window.RtunnelFs

const props = defineProps({
  devid: { type: String, default: '' },
  sys: { type: Object, default: null },
  canEdit: { type: Boolean, default: false },
  embedded: { type: Boolean, default: false }   // 嵌在 tab 里时不画 .panel 与外层标题
})

const caps = computed(() => (props.sys && props.sys.caps) || {})
const fsOn = computed(() => !!caps.value.fs)
const writable = computed(() => !!caps.value.fs_rw && props.canEdit)
const root = computed(() => caps.value.fs_root || '/opt/data/files')
const maxMb = computed(() => caps.value.fs_max_mb || 512)

const space = ref(null)
const usage = computed(() => {
  const s = space.value
  if (!s || !s.total_kb) return ''
  const used = Math.max(0, s.total_kb - s.free_kb)
  return '已用 ' + bytes(used * 1024) + ' / ' + bytes(s.total_kb * 1024)
})

let fm = null
const expanded = ref(false)
const fsHost = ref(null)

function ensureManager() {
  if (fm) return fm
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://'
  fm = RtunnelFs.createManager({
    inline: true,
    container: fsHost.value,
    wsUrl: proto + location.host + '/ws/fs/' + encodeURIComponent(props.devid),
    title: '文件管理 - ' + props.devid,
    editorUrl,
    onClose: () => { expanded.value = false },
    onSpace: (s) => { space.value = s }
  })
  return fm
}

function toggle() {
  if (!props.devid || !fsOn.value) return
  ensureManager()
  if (expanded.value) {
    fm.close()
    expanded.value = false
    return
  }
  expanded.value = true
  fm.open()
  nextTick(() => { if (fsHost.value) fsHost.value.scrollIntoView({ block: 'nearest' }) })
}

// 能力配置里有文件管理时只更新状态，不自动展开；列表在首次展开时才连接加载
watch(fsOn, (on) => {
  if (on) return
  space.value = null
  if (fm) {
    fm.close()
    expanded.value = false
  }
}, { immediate: true })

// 同一个路由组件会在设备之间复用，换设备时重建会话，避免连到上一个设备
watch(() => props.devid, () => {
  space.value = null
  expanded.value = false
  if (!fm) return
  fm.destroy()
  fm = null
})

onBeforeUnmount(() => {
  if (fm) {
    fm.destroy()
    fm = null
  }
})
</script>

<style scoped>
.tab-body { min-width: 0; }
.tab-toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
}
.fs-card {
  padding: 4px 0 2px;
}
.fs-line {
  color: var(--muted);
  font-size: 12px;
  overflow-wrap: anywhere;
}
.fs-host {
  margin-top: 10px;
}
</style>
