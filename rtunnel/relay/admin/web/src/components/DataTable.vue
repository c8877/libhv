<template>
  <el-table ref="el" v-bind="$attrs" :data="view" :header-cell-class-name="headerClass"
            @header-click="onHeaderClick">
    <slot />
    <template v-for="name in forwarded" #[name]="scope">
      <slot :name="name" v-bind="scope || {}" />
    </template>
  </el-table>
</template>

<script setup>
import { computed, reactive, ref, useSlots, watch } from 'vue'
import { Table } from '@/shared/table.js'

defineOptions({ inheritAttrs: false })

const props = defineProps({
  data: { type: Array, default: () => [] },
  // 表头 label/prop -> 排序定义：'字段名' | row => 值 | { key, type, value }
  sortKeys: { type: Object, default: () => ({}) },
  defaultKey: { type: String, default: '' },
  defaultDir: { type: Number, default: 0 },
  // 受控排序：父组件排序后再分页时传这两个，配合 server-sort 只收事件
  sortKey: { type: String, default: null },
  sortDir: { type: Number, default: null },
  // 服务端已排序/分页时只抛事件，不在前端重排
  serverSort: { type: Boolean, default: false }
})
const emit = defineEmits(['sort-change'])
const slots = useSlots()
const el = ref(null)
const sort = reactive({
  key: props.sortKey != null ? props.sortKey : props.defaultKey,
  dir: props.sortDir != null ? props.sortDir : props.defaultDir
})
const active = ref(null)

watch(() => [props.sortKey, props.sortDir], ([k, d]) => {
  if (k == null) return
  sort.key = k
  sort.dir = d || 0
})

const forwarded = computed(() => Object.keys(slots).filter((k) => k !== 'default'))

function specFor(column) {
  if (!column) return null
  const raw = props.sortKeys[column.property] ?? props.sortKeys[column.label]
  if (!raw) return null
  if (typeof raw === 'string') {
    return { key: raw, type: 'text' }
  }
  if (typeof raw === 'function') {
    return { key: column.property || column.label, type: 'text', value: raw }
  }
  return { key: raw.key || column.property || column.label, type: raw.type || 'text', value: raw.value }
}

const view = computed(() => {
  if (props.serverSort || !sort.key || !active.value || active.value.key !== sort.key) return props.data
  return Table.sortRows(props.data, sort, [active.value])
})

function headerClass({ column }) {
  const spec = specFor(column)
  if (!spec) return ''
  return 'dt-sortable' + (sort.key === spec.key ? (sort.dir > 0 ? ' dt-asc' : ' dt-desc') : '')
}

function onHeaderClick(column) {
  const spec = specFor(column)
  if (!spec) return
  Table.toggle(sort, spec.key, spec.type)
  active.value = spec
  emit('sort-change', { key: sort.key, dir: sort.dir, column: column.property || column.label })
}

function clearSelection() {
  el.value && el.value.clearSelection()
}

defineExpose({ el, clearSelection })
</script>

<style>
.el-table__header th.dt-sortable { cursor: pointer; user-select: none; }
.el-table__header th.dt-sortable .cell::after {
  content: '▲'; font-size: 9px; margin-left: 3px; opacity: 0;
}
.el-table__header th.dt-asc .cell::after { content: '▲'; opacity: .6; }
.el-table__header th.dt-desc .cell::after { content: '▼'; opacity: .6; }
</style>
