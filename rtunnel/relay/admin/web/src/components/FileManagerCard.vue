<template>
  <div class="panel">
    <div class="panel-head">
      <h2>文件管理</h2>
      <span class="spacer" />
      <el-tag v-if="sys && fsOn" :type="writable ? 'success' : 'info'" size="small">{{ writable ? '读写' : '只读' }}</el-tag>
      <el-button type="primary" size="small" :disabled="!sys || !fsOn" @click="toggle">{{ expanded ? '收起文件管理' : '打开文件管理' }}</el-button>
    </div>
    <div class="fs-card">
      <div class="mono fs-line">
        <template v-if="!sys">正在读取设备能力…</template>
        <template v-else-if="fsOn">{{ root }} · 单文件上限 {{ maxMb }} MB<span v-if="usage"> · {{ usage }}</span><span v-if="!writable"> · 当前角色只能浏览和下载</span></template>
        <template v-else>该设备未启用文件服务（可在「能力配置」中开启；旧版 agent 需先升级）</template>
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

const RtunnelFs = window.RtunnelFs

const props = defineProps({
  devid: { type: String, default: '' },
  sys: { type: Object, default: null },
  canEdit: { type: Boolean, default: false }
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
    subtitle: (props.sys && props.sys.machine) || '',
    editorUrl,
    onClose: () => { expanded.value = false },
    onSpace: (s) => { space.value = s }
  })
  return fm
}

function open() {
  if (!props.devid || !fsOn.value || expanded.value) return
  expanded.value = true
  ensureManager().open()
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

// 能力配置里有文件管理时直接展开表格，不再要求点按钮
watch(fsOn, (on) => {
  if (!on) {
    space.value = null
    if (fm) {
      fm.close()
      expanded.value = false
    }
    return
  }
  nextTick(open)
}, { immediate: true })

onBeforeUnmount(() => {
  if (fm) {
    fm.destroy()
    fm = null
  }
})
</script>

<style scoped>
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
