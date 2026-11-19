<template>
  <div :class="embedded ? 'tab-body' : 'panel'">
    <div :class="embedded ? 'tab-toolbar' : 'panel-head'">
      <h2 v-if="!embedded">服务管理</h2>
      <el-tag v-if="sys && svcOn" :type="writable ? 'success' : 'info'" size="small">{{ writable ? '可控制' : '只读' }}</el-tag>
      <span class="spacer" />
      <el-button v-if="!embedded" type="primary" size="small" :disabled="!sys || !svcOn" @click="toggle">{{ expanded ? '收起服务管理' : '打开服务管理' }}</el-button>
    </div>
    <div class="svc-card">
      <div class="mono svc-line">
        <template v-if="!sys">正在读取设备能力…</template>
        <template v-else-if="svcOn">systemd 服务管理<span v-if="!writable"> · 当前角色只能查看和读日志</span></template>
        <template v-else>该设备未启用 systemd 服务管理</template>
      </div>
    </div>
    <div ref="svcHost" class="svc-host"></div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import '../../../../../shared/web/svc-ui.js'

const RtunnelSvc = window.RtunnelSvc

const props = defineProps({
  devid: { type: String, default: '' },
  sys: { type: Object, default: null },
  canEdit: { type: Boolean, default: false },
  embedded: { type: Boolean, default: false }   // 嵌在 tab 里时不画 .panel 与外层标题
})

const caps = computed(() => (props.sys && props.sys.caps) || {})
const svcOn = computed(() => !!caps.value.svc)
const writable = computed(() => !!caps.value.svc_rw && props.canEdit)

let sm = null
const expanded = ref(false)
const svcHost = ref(null)

function ensureManager() {
  if (sm) return sm
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://'
  sm = RtunnelSvc.createManager({
    inline: true,
    container: svcHost.value,
    wsUrl: proto + location.host + '/ws/svc/' + encodeURIComponent(props.devid),
    title: '服务管理 - ' + props.devid,
    onClose: () => { expanded.value = false }
  })
  return sm
}

function toggle() {
  if (!props.devid || !svcOn.value) return
  ensureManager()
  if (expanded.value) {
    sm.close()
    expanded.value = false
    return
  }
  expanded.value = true
  sm.open()
  nextTick(() => { if (svcHost.value) svcHost.value.scrollIntoView({ block: 'nearest' }) })
}

// 嵌在 tab 里时，切换到该 tab 就等于打开服务管理
function autoOpen() {
  if (!props.embedded || !props.devid || !svcOn.value) return
  ensureManager()
  if (!expanded.value) {
    expanded.value = true
    sm.open()
  }
}

// 设备不支持时收起；同一路由组件在设备之间复用，换设备时重建会话
watch(svcOn, (on) => {
  if (on) {
    nextTick(autoOpen)
    return
  }
  if (sm) {
    sm.close()
    expanded.value = false
  }
}, { immediate: true })

watch(() => props.devid, () => {
  expanded.value = false
  if (sm) {
    sm.destroy()
    sm = null
  }
  nextTick(autoOpen)
})

onMounted(() => { nextTick(autoOpen) })

onBeforeUnmount(() => {
  if (sm) {
    sm.destroy()
    sm = null
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
.svc-card {
  padding: 4px 0 2px;
}
.svc-line {
  color: var(--muted);
  font-size: 12px;
  overflow-wrap: anywhere;
}
.svc-host {
  margin-top: 10px;
}
</style>
