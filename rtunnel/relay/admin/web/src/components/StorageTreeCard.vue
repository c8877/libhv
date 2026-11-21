<template>
  <div ref="host" class="store-host"></div>
</template>

<script setup>
// 存储目录树：真正的实现在 shared/web/storage-ui.js，与设备本地页共用一份。
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import '../../../../../shared/web/storage-ui.js'

const RtunnelStorage = window.RtunnelStorage

const props = defineProps({
  mounts: { type: Array, default: () => [] },
  // (mount, path, depth) => Promise<STORAGE 响应>
  fetchNodes: { type: Function, required: true },
})

const host = ref(null)
let tree = null

function ensure() {
  if (tree) return tree
  tree = RtunnelStorage.createTree({ container: host.value, fetchNodes: props.fetchNodes })
  return tree
}

watch(() => props.mounts, (v) => { ensure().setMounts(v || []) })

onMounted(() => { ensure().setMounts(props.mounts || []) })

onBeforeUnmount(() => {
  if (tree) {
    tree.destroy()
    tree = null
  }
})
</script>

<style scoped>
/* 把 Element Plus 主题变量映射给共享组件，深浅色跟随管理台 */
.store-host {
  min-width: 0;
  --rt-fg: var(--el-text-color-primary);
  --rt-bg: var(--el-bg-color-overlay);
  --rt-line: var(--el-border-color-lighter);
  --rt-muted: var(--el-text-color-secondary);
  --rt-hover: var(--el-fill-color-light);
  --rt-fill: var(--el-fill-color);
  --rt-fill-dark: var(--el-fill-color-darker);
  --rt-accent: var(--el-color-primary);
}
</style>
