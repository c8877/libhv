<template>
  <div class="panel">
    <div class="panel-head">
      <h2>用量</h2>
      <span class="muted">共 {{ rows.length }} 行 · 合计 {{ fmtQty(total, metric) }}</span>
      <span class="spacer" />
      <el-select v-if="can('tenant.read')" v-model="tenantId" style="width: 150px" @change="load">
        <el-option label="全部租户" :value="0" />
        <el-option v-for="t in tenants" :key="t.id" :label="t.name" :value="t.id" />
      </el-select>
      <el-select v-model="metric" style="width: 150px" @change="load">
        <el-option v-for="m in METRICS" :key="m.value" :label="m.label" :value="m.value" />
      </el-select>
      <el-date-picker v-model="range" type="daterange" value-format="YYYY-MM-DD"
                      start-placeholder="开始日" end-placeholder="结束日"
                      style="width: 240px" @change="load" />
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>

    <el-table :data="rows" v-loading="loading" size="small">
      <el-table-column prop="day" label="日期" width="120" sortable :sort-orders="TEXT_ORDERS" />
      <el-table-column v-if="tenantId === 0" label="租户" min-width="110">
        <template #default="{ row }">{{ tenantName(row.tenant_id) }}</template>
      </el-table-column>
      <el-table-column label="主体" min-width="200">
        <template #default="{ row }">
          <el-tag size="small" effect="plain">{{ subjectLabel(row.subject_type) }}</el-tag>
          <span class="mono">{{ row.subject_id }}</span>
        </template>
      </el-table-column>
      <el-table-column label="指标" width="150">
        <template #default="{ row }">{{ metricLabel(row.metric) }}</template>
      </el-table-column>
      <el-table-column label="用量" width="140" align="right">
        <template #default="{ row }">{{ fmtQty(row.qty, row.metric) }}</template>
      </el-table-column>
      <el-table-column prop="events" label="事件数" width="100" align="right"
                       sortable :sort-orders="NUM_ORDERS" />
      <template #empty><el-empty description="所选范围内没有用量" :image-size="70" /></template>
    </el-table>

    <div class="hint muted">
      日按 relay 本地时区；流量统计隧道载荷字节（双向），会话按次与秒，设备在线按秒。
      原始流水保留 180 天（设置项 <code>usage.raw_retention_days</code>），日聚合长期保留。
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'
import { bytesIec, spanText } from '@/shared/utils.js'
import { useTenants } from '@/shared/tenants.js'

const METRICS = [
  { value: 'bytes', label: '隧道流量' },
  { value: 'sessions', label: '会话次数' },
  { value: 'session_seconds', label: '会话时长' },
  { value: 'online_seconds', label: '设备在线时长' },
]

const rows = ref([])
const { tenants, loadTenants, tenantName } = useTenants()
const tenantId = ref(0)
const loading = ref(false)
const metric = ref('bytes')
const range = ref(defaultRange())

function defaultRange() {
  const end = new Date()
  const start = new Date(end.getTime() - 6 * 86400000)
  const fmt = (d) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
  return [fmt(start), fmt(end)]
}

const total = computed(() => rows.value.reduce((n, r) => n + (Number(r.qty) || 0), 0))

function metricLabel(key) {
  const m = METRICS.find((x) => x.value === key)
  return m ? m.label : key
}

function subjectLabel(t) {
  return t === 'token' ? '令牌' : t === 'device' ? '设备' : t
}

function fmtQty(qty, key) {
  if (key === 'bytes') return bytesIec(qty)
  if (key === 'session_seconds' || key === 'online_seconds') return spanText(qty)
  return String(qty)
}

async function load() {
  loading.value = true
  try {
    const q = new URLSearchParams()
    q.set('metric', metric.value)
    q.set('granularity', 'day')
    if (tenantId.value > 0) q.set('tenant_id', String(tenantId.value))
    if (range.value && range.value.length === 2) {
      q.set('from', range.value[0])
      q.set('to', range.value[1])
    }
    const res = await Api.get('/api/v1/usage?' + q.toString())
    rows.value = (res && res.rows) || []
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  loadTenants()
  load()
})
</script>

<style scoped>
.mono { margin-left: 8px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
.hint { margin-top: 10px; font-size: 12.5px; line-height: 1.7; }
.hint code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; row-gap: 6px; }
}
</style>
