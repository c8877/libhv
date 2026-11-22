<template>
  <div :class="embedded ? 'tab-body' : 'panel'">
    <div v-if="!embedded" class="panel-head">
      <h2>文件管理</h2>
      <el-tag v-if="sys && fsOn" :type="writable ? 'success' : 'info'" size="small">{{ writable ? '读写' : '只读' }}</el-tag>
      <span class="spacer" />
      <el-button type="primary" size="small" :disabled="!sys || !fsOn" @click="toggle">{{ expanded ? '收起文件管理' : '打开文件管理' }}</el-button>
    </div>
    <div v-if="tabState" class="tab-empty">{{ tabState }}</div>
    <div v-show="!tabState" ref="fsHost" class="fs-host"></div>
    <div v-if="!embedded" class="fs-card">
      <div class="mono fs-line">
        <template v-if="!sys">正在读取设备能力…</template>
        <template v-else-if="fsOn">{{ root }} · 单文件上限 {{ maxMb }} MB<span v-if="usage"> · {{ usage }}</span><span v-if="!writable"> · 当前角色只能浏览和下载</span></template>
        <template v-else>该设备未启用文件服务（可在「能力配置」中开启）</template>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { bytes } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import editorUrl from '../../../../../shared/web/cm-editor.bundle.js?url'
import '../../../../../shared/web/table-ui.js'
import '../../../../../shared/web/fs-ui.js'
import '../../../../../shared/web/file-view.js'

const RtunnelFs = window.RtunnelFs
const { isPhone } = useBreakpoint()

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

// 嵌在 tab 里时不再画说明条：能力未就绪/未开启展示空状态，正常时信息进管理器工具栏
const tabState = computed(() => {
  if (!props.embedded) return ''
  if (!props.sys) return '正在读取设备能力…'
  if (!fsOn.value) return '该设备未启用文件服务（可在「能力配置」中开启）'
  return ''
})

const noteText = computed(() => {
  if (!fsOn.value) return ''
  // 手机宽度放不下三段：省略「上限」，只留根目录和已用空间
  let s = root.value
  if (!isPhone.value) s += ' · 上限 ' + maxMb.value + ' MB'
  if (usage.value) s += ' · ' + usage.value
  else if (isPhone.value) s += ' · 上限 ' + maxMb.value + ' MB'
  if (!writable.value) s += ' · 只读'
  return s
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
    note: noteText.value,
    noteWarn: !writable.value,
    onClose: () => { expanded.value = false },
    onSpace: (s) => { space.value = s }
  })
  return fm
}

// 根目录、上限、已用空间或只读状态变化时同步工具栏说明
watch(noteText, (t) => { if (fm) fm.setNote(t, !writable.value) })

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

// 嵌在 tab 里时，切换到该 tab 就等于打开文件管理
function autoOpen() {
  if (!props.embedded || !props.devid || !fsOn.value) return
  ensureManager()
  if (!expanded.value) {
    expanded.value = true
    fm.open()
  }
}

// 能力配置里有文件管理时只更新状态，不自动展开；列表在首次展开时才连接加载
watch(fsOn, (on) => {
  if (on) {
    nextTick(autoOpen)
    return
  }
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
  if (fm) {
    fm.destroy()
    fm = null
  }
  nextTick(autoOpen)
})

onMounted(() => { nextTick(autoOpen) })

onBeforeUnmount(() => {
  if (fm) {
    fm.destroy()
    fm = null
  }
})
</script>

<style scoped>
.tab-body { min-width: 0; }
.tab-empty {
  padding: 48px 16px;
  text-align: center;
  color: var(--el-text-color-secondary);
  font-size: 13px;
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
.tab-body .fs-host { margin-top: 0; }
</style>
