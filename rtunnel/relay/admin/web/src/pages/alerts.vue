<template>
  <div class="panel">
    <div class="panel-head">
      <h2>告警</h2>
      <span class="muted">
        未确认 {{ counts.open || 0 }} · 严重 {{ counts.critical || 0 }} · 共 {{ counts.total || 0 }} 条
      </span>
      <span class="spacer" />
      <el-button :loading="loading" @click="load">刷新</el-button>
      <el-button v-if="can('alert.write')" type="primary" :disabled="!counts.open"
                 :loading="acking" @click="ackAll">
        全部确认
      </el-button>
    </div>

    <div class="bar">
      <el-radio-group v-model="state" size="small" @change="load">
        <el-radio-button value="open">未确认 {{ counts.open || 0 }}</el-radio-button>
        <el-radio-button value="acked">已确认 {{ (counts.total || 0) - (counts.open || 0) }}</el-radio-button>
        <el-radio-button value="all">全部 {{ counts.total || 0 }}</el-radio-button>
      </el-radio-group>
      <span class="spacer" />
      <span class="muted">设备上下线、证书到期、任务失败会在这里出现；通知渠道在「设置 → 告警」</span>
    </div>

    <el-table :data="pageRows" v-loading="loading" size="small" row-key="id">
      <template v-if="isPhone">
        <el-table-column label="告警">
          <template #default="{ row }">
            <div class="ev">
              <el-tag :type="levelType(row.level)" size="small" effect="plain">{{ levelText(row.level) }}</el-tag>
              <b>{{ row.title }}</b>
            </div>
            <div class="ev-sub">{{ row.text }}</div>
            <div class="ev-sub muted">{{ when(row.at_ms) }}<template v-if="row.target"> · {{ row.target }}</template></div>
            <el-button v-if="can('alert.write') && !row.acked_ms" link type="primary" size="small"
                       @click="ack(row)">确认</el-button>
          </template>
        </el-table-column>
      </template>
      <template v-else>
        <el-table-column label="时间" width="150">
          <template #default="{ row }"><span class="muted">{{ when(row.at_ms) }}</span></template>
        </el-table-column>
        <el-table-column label="级别" width="80">
          <template #default="{ row }">
            <el-tag :type="levelType(row.level)" size="small" effect="plain">{{ levelText(row.level) }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="告警" min-width="240">
          <template #default="{ row }">
            <div>{{ row.title }}</div>
            <div class="muted">{{ row.text }}</div>
          </template>
        </el-table-column>
        <el-table-column label="对象" min-width="140">
          <template #default="{ row }">
            <span v-if="row.target" class="mono">{{ row.target }}</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="150">
          <template #default="{ row }">
            <template v-if="row.acked_ms">
              <span class="muted">{{ when(row.acked_ms) }}</span>
              <div class="muted">{{ row.acked_by }}</div>
            </template>
            <el-tag v-else type="warning" size="small" effect="plain">未确认</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="90">
          <template #default="{ row }">
            <el-button v-if="can('alert.write') && !row.acked_ms" link type="primary"
                       size="small" @click="ack(row)">确认</el-button>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
      </template>
      <template #empty>
        <el-empty :description="emptyText" :image-size="70">
          <el-button v-if="state === 'open' && counts.total" @click="state = 'all'; load()">看历史</el-button>
        </el-empty>
      </template>
    </el-table>

    <div class="foot">
      <el-pagination v-model:current-page="page" :page-size="per" :total="rows.length"
                     layout="prev, pager, next, total" background />
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { Api } from '@/shared/api.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { can } from '@/shared/store.js'

const { isPhone } = useBreakpoint()

const state = ref('open')
const rows = ref([])
const counts = ref({})
const loading = ref(false)
const acking = ref(false)
const page = ref(1)
const per = 20

const pageRows = computed(() => rows.value.slice((page.value - 1) * per, page.value * per))
const emptyText = computed(() => {
  if (state.value === 'open') return counts.value.total ? '没有未确认的告警' : '还没有告警'
  if (state.value === 'acked') return '还没有确认过的告警'
  return '还没有告警'
})

watch(state, () => { page.value = 1 })

function pad(n) {
  return String(n).padStart(2, '0')
}

function when(ms) {
  if (!ms) return '—'
  const d = new Date(Number(ms))
  if (isNaN(d.getTime())) return '—'
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
    `${pad(d.getHours())}:${pad(d.getMinutes())}`
}

function levelType(l) {
  return l === 'critical' ? 'danger' : l === 'info' ? 'info' : 'warning'
}

function levelText(l) {
  return l === 'critical' ? '严重' : l === 'info' ? '提示' : '警告'
}

const LEVEL_RANK = { critical: 3, warning: 2, info: 1 }

function normalize(row) {
  // 对象：任务告警看任务号，设备告警看 devid，证书看 san（task.failed 同时带 devid，
  // 但运维要找的是那条任务，所以任务类优先）
  const d = row.detail && typeof row.detail === 'object' ? row.detail : {}
  let target = ''
  if (String(row.event || '').startsWith('task.') && d.task_id) target = '任务 #' + d.task_id
  else if (d.devid) target = 'devid:' + d.devid
  else if (d.san) target = 'cert:' + d.san
  else if (d.task_id) target = '任务 #' + d.task_id
  return {
    ...row,
    target,
    rank: LEVEL_RANK[row.level] || 0,
  }
}

async function load() {
  loading.value = true
  try {
    const r = await Api.get(`/api/v1/alerts?state=${state.value}&limit=200`)
    rows.value = (r.items || []).map(normalize)
    counts.value = r.counts || {}
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function ack(row) {
  try {
    await Api.post(`/api/v1/alerts/${row.id}/ack`, {})
    ElMessage.success('已确认')
    await load()
  } catch (e) {
    ElMessage.error('确认失败：' + e.message)
  }
}

async function ackAll() {
  acking.value = true
  try {
    const r = await Api.post('/api/v1/alerts/ack-all', {})
    ElMessage.success('已确认 ' + (r.acked || 0) + ' 条')
    await load()
  } catch (e) {
    ElMessage.error('确认失败：' + e.message)
  } finally {
    acking.value = false
  }
}

// 告警是“等你看”的东西：页面打开期间每 15 秒刷一次，不用手动点
let timer = null
onMounted(() => {
  load()
  timer = setInterval(() => { if (state.value === 'open') load() }, 15000)
})
onBeforeUnmount(() => clearInterval(timer))
</script>

<style scoped>
.bar { display: flex; align-items: center; gap: 10px; margin: -4px 0 10px; flex-wrap: wrap; }
.foot { display: flex; justify-content: flex-end; margin-top: 12px; }
.ev { display: flex; align-items: center; gap: 8px; }
.ev-sub { margin-top: 2px; font-size: 12px; word-break: break-all; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
}
</style>
