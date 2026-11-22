<template>
  <div class="dict-chips" :class="{ readonly: !canEdit }">
    <div class="dc-bar">
      <span class="dc-count" :class="{ full: count >= max }">已选 {{ count }} / {{ max }}</span>
      <span class="dc-spacer" />
      <el-link v-if="manageTo" type="primary" :underline="false" @click="$emit('manage')">
        管理字典 ↗
      </el-link>
    </div>

    <div class="dc-box">
      <el-input v-if="needSearch" v-model="query" size="small" clearable class="dc-search"
                :placeholder="searchPlaceholder" />

      <div class="dc-list" :class="{ clamped: clamped }">
        <el-checkbox v-for="opt in viewOptions" :key="opt.key"
                     class="dc-chip" :class="[stateOf(opt.key), { off: !opt.enabled }]"
                     :model-value="stateOf(opt.key) !== 'none'"
                     :indeterminate="stateOf(opt.key) === 'some'"
                     :disabled="!canEdit || (!opt.enabled && stateOf(opt.key) === 'none')"
                     :title="titleOf(opt)" @change="toggle(opt)">
          <span class="dc-label">{{ opt.label }}</span>
          <span v-if="opt.sub && opt.sub !== opt.label" class="dc-sub">{{ opt.sub }}</span>
          <button v-if="canEdit && stateOf(opt.key) !== 'none'" class="dc-x" type="button"
                  aria-label="移除" @click.stop.prevent="clearOption(opt)">×</button>
        </el-checkbox>

        <span v-for="v in customValues" :key="'c' + v" class="dc-chip all custom"
              :title="customTitle(v)">
          <span class="dc-check">✓</span>
          <span class="dc-label">{{ v }}</span>
          <button v-if="canEdit" class="dc-x" type="button" aria-label="移除"
                  @click.stop="removeValue(v)">×</button>
        </span>

        <span v-if="!viewOptions.length && !customValues.length" class="dc-empty muted">
          {{ query ? '没有匹配的条目' : emptyHint }}
        </span>
      </div>

      <div class="dc-foot">
        <el-button v-if="foldable" link size="small" @click="expanded = !expanded">
          {{ expanded || query ? '收起' : '展开全部（' + viewOptions.length + ' 项）' }}
        </el-button>
        <span v-if="!canEdit" class="muted">没有修改权限</span>
        <span class="dc-spacer" />
        <el-popover v-model:visible="customOpen" placement="bottom-end" :width="300" trigger="click">
          <template #reference>
            <el-button size="small" :disabled="!canEdit || count >= max">+ 自定义</el-button>
          </template>
          <div class="dc-custom">
            <div class="dc-custom-tip">{{ customHint }}</div>
            <el-input v-model="customInput" size="small" :placeholder="placeholder"
                      @keyup.enter="addCustom" />
            <div v-if="customNote" class="dc-custom-note" :class="{ bad: customOver }">{{ customNote }}</div>
            <div class="dc-custom-actions">
              <el-button size="small" @click="customOpen = false">取消</el-button>
              <el-button size="small" type="primary" :disabled="customOver || !customValuesParsed.length"
                         @click="addCustom">添加</el-button>
            </div>
          </div>
        </el-popover>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'

const props = defineProps({
  // 已选值：端口为 number[]，串口为 string[]
  modelValue: { type: Array, default: () => [] },
  // [{ key, label, sub, values: [], enabled, dict }]
  options: { type: Array, default: () => [] },
  max: { type: Number, default: 31 },
  canEdit: { type: Boolean, default: false },
  kind: { type: String, default: 'port' }, // port | tty
  manageTo: { type: String, default: '' },
  placeholder: { type: String, default: '' },
  emptyHint: { type: String, default: '字典暂无可选项，可用「+ 自定义」添加' },
})
const emit = defineEmits(['update:modelValue', 'manage'])

const query = ref('')
const expanded = ref(false)
const customOpen = ref(false)
const customInput = ref('')

const count = computed(() => props.modelValue.length)
const selSet = computed(() => new Set(props.modelValue))
const unit = computed(() => (props.kind === 'tty' ? '串口' : '端口'))

const states = computed(() => {
  const m = new Map()
  for (const opt of props.options) {
    let hit = 0
    for (const v of opt.values) if (selSet.value.has(v)) hit++
    m.set(opt.key, hit === 0 ? 'none' : hit === opt.values.length ? 'all' : 'some')
  }
  return m
})
function stateOf(key) {
  return states.value.get(key) || 'none'
}

// 设备上已有、但字典里查不到的值，原样展示并允许移除
const covered = computed(() => {
  const s = new Set()
  for (const opt of props.options) for (const v of opt.values) s.add(v)
  return s
})
const customValues = computed(() => props.modelValue.filter((v) => !covered.value.has(v)))

const filtered = computed(() => {
  const k = query.value.trim().toLowerCase()
  if (!k) return props.options
  return props.options.filter((o) =>
    String(o.label || '').toLowerCase().includes(k) ||
    String(o.sub || '').toLowerCase().includes(k) ||
    o.values.some((v) => String(v).includes(k)))
})
// 已选排在前，其次可用项，最后字典停用项
const viewOptions = computed(() => {
  const rank = (o) => (stateOf(o.key) !== 'none' ? 0 : o.enabled ? 1 : 2)
  return filtered.value
    .map((o, i) => ({ o, i }))
    .sort((a, b) => rank(a.o) - rank(b.o) || a.i - b.i)
    .map((x) => x.o)
})
const needSearch = computed(() => props.options.length > 12)
const foldable = computed(() => filtered.value.length > 12)
const clamped = computed(() => foldable.value && !expanded.value && !query.value)
const searchPlaceholder = computed(() => (props.kind === 'tty' ? '搜索串口或名称' : '搜索端口或名称'))

watch(query, () => { expanded.value = false })
watch(customOpen, (v) => { if (!v) { customInput.value = '' } })

function titleOf(opt) {
  if (!opt.enabled) return opt.sub + '（字典已停用，仍在本机生效）'
  return opt.sub || opt.label
}
function customTitle(v) {
  return v + '（字典中没有，保存时按原值下发）'
}

function setValue(list) {
  // 端口按数值排序去重，串口保持添加顺序
  if (props.kind === 'port') {
    const uniq = Array.from(new Set(list.map(Number)))
    uniq.sort((a, b) => a - b)
    emit('update:modelValue', uniq)
  } else {
    emit('update:modelValue', Array.from(new Set(list)))
  }
}

function toggle(opt) {
  if (!props.canEdit) return
  const st = stateOf(opt.key)
  if (st === 'all') {
    setValue(props.modelValue.filter((v) => !opt.values.includes(v)))
    return
  }
  if (st === 'none' && !opt.enabled) return
  const add = opt.values.filter((v) => !selSet.value.has(v))
  if (count.value + add.length > props.max) {
    ElMessage.warning(`最多 ${props.max} 个${unit.value}，当前已选 ${count.value}，该项需要 ${add.length} 个`)
    return
  }
  setValue(props.modelValue.concat(add))
}

function clearOption(opt) {
  if (!props.canEdit) return
  setValue(props.modelValue.filter((v) => !opt.values.includes(v)))
}

function removeValue(v) {
  if (!props.canEdit) return
  setValue(props.modelValue.filter((x) => x !== v))
}

const customValuesParsed = computed(() => {
  const raw = customInput.value.trim()
  if (!raw) return []
  if (props.kind === 'tty') return raw[0] === '/' && !raw.includes('..') ? [raw] : []
  const range = raw.match(/^(\d+)\s*-\s*(\d+)$/)
  if (range) {
    const lo = Number(range[1])
    const hi = Number(range[2])
    if (!(lo >= 1 && hi <= 65535 && lo <= hi)) return []
    if (hi - lo + 1 > props.max) return [{ tooMany: hi - lo + 1 }]
    const out = []
    for (let p = lo; p <= hi; p++) out.push(p)
    return out
  }
  if (!/^\d+$/.test(raw)) return []
  const p = Number(raw)
  return p >= 1 && p <= 65535 ? [p] : []
})
const customOver = computed(() => {
  const list = customValuesParsed.value
  if (list.length === 1 && list[0] && list[0].tooMany) return true
  const add = list.filter((v) => !selSet.value.has(v)).length
  return count.value + add > props.max
})
const customHint = computed(() =>
  props.kind === 'tty'
    ? '字典里没有的串口路径，需以 / 开头，例如 /dev/ttyS3'
    : '字典里没有的端口，可填单个端口或区间，例如 9000 或 8000-8010')
const customNote = computed(() => {
  const list = customValuesParsed.value
  if (!customInput.value.trim()) return ''
  if (!list.length) return '格式不正确'
  if (list[0] && list[0].tooMany) {
    return `区间含 ${list[0].tooMany} 个端口，超过上限 ${props.max}`
  }
  const add = list.filter((v) => !selSet.value.has(v)).length
  const after = count.value + add
  if (props.kind === 'tty') return add ? `将添加 ${add} 个${unit.value}，保存后 ${after} / ${props.max}` : '已在本机白名单中'
  return `将添加 ${add} 个${unit.value}，保存后 ${after} / ${props.max}`
})

function addCustom() {
  const list = customValuesParsed.value
  if (!list.length || customOver.value) return
  setValue(props.modelValue.concat(list.filter((v) => !selSet.value.has(v))))
  customOpen.value = false
  customInput.value = ''
}
</script>

<style scoped>
.dict-chips { width: 100%; max-width: 720px; }
.dc-bar { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; font-size: 12px; color: var(--el-text-color-secondary); }
.dc-count.full { color: var(--el-color-danger); }
.dc-spacer { flex: 1; }
.dc-box { padding: 8px; border: 1px solid var(--el-border-color); border-radius: 6px; background: var(--el-fill-color-blank); }
.dc-search { margin-bottom: 8px; }
.dc-list { display: flex; flex-wrap: wrap; gap: 6px; }
.dc-list.clamped { max-height: 94px; overflow: hidden; }
.dc-empty { font-size: 12px; }
.dc-chip {
  display: inline-flex; align-items: center; gap: 4px; max-width: 100%; height: 24px;
  margin-right: 0; padding: 0 8px; border: 1px solid var(--el-border-color); border-radius: 12px;
  background: var(--el-fill-color-light); font-size: 12px; line-height: 1;
}
.dc-chip :deep(.el-checkbox__input) { height: 14px; }
.dc-chip :deep(.el-checkbox__inner) { width: 12px; height: 12px; border-radius: 3px; }
.dc-chip :deep(.el-checkbox__inner::after) { left: 4px; top: 1px; height: 6px; }
.dc-chip :deep(.el-checkbox__label) {
  display: inline-flex; align-items: center; gap: 4px;
  font-size: 12px; line-height: 1; color: var(--el-text-color-regular);
}
.dc-chip:hover { border-color: var(--el-color-primary-light-5); }
.dc-chip.all { background: var(--el-color-primary-light-9); border-color: var(--el-color-primary-light-5); }
.dc-chip.all :deep(.el-checkbox__label) { color: var(--el-color-primary); }
.dc-chip.some { background: var(--el-color-primary-light-9); border-style: dashed; border-color: var(--el-color-primary-light-5); }
.dc-chip.some :deep(.el-checkbox__label) { color: var(--el-color-primary); }
.dc-chip.off { border-style: dashed; background: transparent; }
.dc-chip.off :deep(.el-checkbox__label) { color: var(--el-text-color-disabled); }
.dc-chip.off:hover { border-color: var(--el-border-color); }
.dc-chip.off.all, .dc-chip.off.some {
  border-style: dashed; border-color: var(--el-border-color);
  background: var(--el-fill-color-light);
}
.dc-chip.off.all :deep(.el-checkbox__label),
.dc-chip.off.some :deep(.el-checkbox__label) { color: var(--el-text-color-secondary); }
.dc-chip.custom { border-style: dashed; border-color: var(--el-color-warning-light-5); background: var(--el-color-warning-light-9); color: var(--el-color-warning); }
.dc-check { font-size: 11px; font-weight: 700; }
.dc-label { max-width: 160px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dc-sub { font-size: 11px; opacity: .75; font-family: ui-monospace, Menlo, Consolas, monospace; }
.dc-x {
  padding: 0 0 0 2px; border: 0; background: transparent; color: inherit;
  font-size: 13px; line-height: 1; opacity: .6; cursor: pointer;
}
.dc-x:hover { opacity: 1; }
.dc-foot { display: flex; align-items: center; gap: 8px; margin-top: 8px; }
.readonly .dc-chip { cursor: default; }
.dc-custom { display: flex; flex-direction: column; gap: 8px; }
.dc-custom-tip { font-size: 12px; color: var(--el-text-color-secondary); line-height: 1.5; }
.dc-custom-note { font-size: 12px; color: var(--el-text-color-secondary); }
.dc-custom-note.bad { color: var(--el-color-danger); }
.dc-custom-actions { display: flex; justify-content: flex-end; gap: 8px; }
@media (max-width: 599px) {
  .dc-label { max-width: 120px; }
  .dc-list.clamped { max-height: 84px; }
}
</style>
