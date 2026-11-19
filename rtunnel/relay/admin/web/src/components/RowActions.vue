<template>
  <div ref="box" class="row-actions" @click.stop>
    <el-button v-for="it in shown" :key="it.key" size="small"
               :type="it.danger ? 'danger' : ''" :plain="!!it.danger"
               @click="emit('action', it.key)">{{ it.label }}</el-button>
    <el-dropdown v-if="hidden.length" trigger="click" @command="(c) => emit('action', c)">
      <el-button size="small" text class="more-btn" aria-label="更多操作">
        <el-icon><MoreFilled /></el-icon>
      </el-button>
      <template #dropdown>
        <el-dropdown-menu>
          <el-dropdown-item v-for="it in hidden" :key="it.key" :command="it.key">{{ it.label }}</el-dropdown-item>
        </el-dropdown-menu>
      </template>
    </el-dropdown>

    <div ref="ghost" class="ghost" aria-hidden="true">
      <el-button v-for="it in items" :key="'g-' + it.key" size="small"
                 :type="it.danger ? 'danger' : ''" :plain="!!it.danger">{{ it.label }}</el-button>
      <el-button size="small" text class="more-btn"><el-icon><MoreFilled /></el-icon></el-button>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'

const props = defineProps({
  items: { type: Array, default: () => [] },
  dropdownOnly: { type: Boolean, default: false },
})
const emit = defineEmits(['action'])

const GAP = 6
const box = ref(null)
const ghost = ref(null)
const fit = ref(0)

const shown = computed(() => (props.dropdownOnly ? [] : props.items.slice(0, fit.value)))
const hidden = computed(() => (props.dropdownOnly ? props.items : props.items.slice(fit.value)))

function measure() {
  const boxEl = box.value
  const ghostEl = ghost.value
  if (!boxEl || !ghostEl) return
  const n = props.items.length
  if (props.dropdownOnly || n === 0) {
    fit.value = 0
    return
  }
  const avail = boxEl.clientWidth
  const kids = Array.from(ghostEl.children)
  if (kids.length < n + 1) return
  const widths = kids.map((k) => k.getBoundingClientRect().width)
  const moreW = widths[n]
  let count = 0
  let used = 0
  for (let i = 0; i < n; i++) {
    const gap = count > 0 ? GAP : 0
    const remain = n - (count + 1)
    const reserve = remain > 0 ? GAP + moreW : 0
    if (used + gap + widths[i] + reserve > avail) break
    used += gap + widths[i]
    count++
  }
  fit.value = count
}

let ro = null
function remeasure() {
  nextTick(measure)
}

watch(() => props.items.map((i) => i.key).join(','), remeasure)
watch(() => props.dropdownOnly, remeasure)

onMounted(() => {
  remeasure()
  if (typeof ResizeObserver !== 'undefined') {
    ro = new ResizeObserver(measure)
    if (box.value) ro.observe(box.value)
  }
})
onBeforeUnmount(() => {
  if (ro) ro.disconnect()
})
</script>

<style scoped>
.row-actions {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  min-width: 0;
}
.row-actions :deep(.el-button + .el-button) { margin-left: 0; }
.more-btn { padding: 4px 6px; }
.ghost {
  position: absolute;
  left: -10000px;
  top: 0;
  display: flex;
  gap: 6px;
  visibility: hidden;
  pointer-events: none;
  white-space: nowrap;
}
</style>
