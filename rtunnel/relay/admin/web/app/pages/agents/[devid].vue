<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>{{ devid }}</h2>
        <el-tag type="success" size="small">在线</el-tag>
        <span class="spacer" />
        <el-switch v-model="auto" active-text="自动刷新" @change="onAutoChange" />
        <el-select v-model="period" class="period" @change="onAutoChange">
          <el-option v-for="p in [5, 10, 30]" :key="p" :label="p + 's'" :value="p" />
        </el-select>
        <el-button :loading="loading" @click="loadAll({ notify: true })">刷新</el-button>
        <el-button @click="exportJson">导出 JSON</el-button>
      </div>

      <el-descriptions v-if="detail" :column="descCols" size="small" border>
        <el-descriptions-item label="远端地址">{{ detail.remote_addr || '—' }}</el-descriptions-item>
        <el-descriptions-item label="接入时间">{{ time(detail.connected_at_ms) }}</el-descriptions-item>
        <el-descriptions-item label="最近活跃">{{ ago(detail.last_seen_ms) }} 前</el-descriptions-item>
        <el-descriptions-item label="流量 ↓/↑">
          {{ bytes(detail.bytes_in) }} / {{ bytes(detail.bytes_out) }}
        </el-descriptions-item>
      </el-descriptions>
    </div>

    <div class="grid-2">
      <div class="panel">
        <div class="panel-head">
          <h2>存储</h2>
          <span class="spacer" />
          <span class="muted">条形按最大分区归一</span>
        </div>
        <div v-for="d in disks" :key="d.mount" class="disk">
          <div class="disk-top">
            <span class="mount">{{ d.mount }}</span>
            <span v-if="d.fstype" class="fs">{{ d.fstype }}</span>
            <span class="pct" :class="toneOf(d.pct)">{{ d.pct }}%</span>
          </div>
          <el-progress :percentage="d.pct" :stroke-width="6" :show-text="false" :color="diskColor(d.pct)" />
          <div class="size">{{ d.used }} / {{ d.total }} {{ d.unit }}</div>
        </div>
        <el-empty v-if="sys && !disks.length" description="无挂载点" :image-size="60" />
        <el-skeleton v-else-if="!sys" :rows="3" animated />
      </div>

      <div class="panel">
        <div class="panel-head"><h2>网卡</h2></div>
        <el-table :data="nics" size="small">
          <el-table-column prop="name" label="接口" min-width="90" />
          <el-table-column prop="ip" label="IP" min-width="130" />
          <el-table-column v-if="!isPhone" prop="netmask" label="掩码" min-width="120" />
          <el-table-column label="状态" width="80">
            <template #default="{ row }">
              <el-tag :type="row.up ? 'success' : 'danger'" size="small">{{ row.up ? 'up' : 'down' }}</el-tag>
            </template>
          </el-table-column>
          <template #empty><div class="muted">暂无网卡信息</div></template>
        </el-table>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head"><h2>系统信息</h2></div>
      <el-descriptions v-if="sys" :column="sysCols" size="small" border>
        <el-descriptions-item label="架构">{{ sys.machine || '—' }}</el-descriptions-item>
        <el-descriptions-item label="内核">{{ sys.kernel || '—' }}</el-descriptions-item>
        <el-descriptions-item label="CPU 型号">{{ sys.cpu_model || '—' }}</el-descriptions-item>
        <el-descriptions-item label="核心数">{{ sys.cpu_count || 0 }} 核</el-descriptions-item>
        <el-descriptions-item label="板卡型号">{{ sys.board || '—' }}</el-descriptions-item>
        <el-descriptions-item label="采集时间">{{ time(sysTs) }}</el-descriptions-item>
      </el-descriptions>
      <el-alert v-else-if="sysErr" :title="sysErr" type="warning" :closable="false" show-icon />
      <el-skeleton v-else :rows="2" animated />
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>会话</h2>
        <span class="muted">共 {{ streams.length }} 条</span>
      </div>
      <el-table :data="streams" size="small">
        <el-table-column label="A 用户:sid" min-width="160">
          <template #default="{ row }">{{ row.a_user }}:{{ row.a_sid }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="c_sid" label="c_sid" min-width="100" />
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="row.opened && !row.a_closed && !row.c_closed ? 'success' : 'info'" size="small">
              {{ row.opened && !row.a_closed && !row.c_closed ? '活跃' : '关闭' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="开启于" min-width="150">
          <template #default="{ row }">{{ time(row.opened_at_ms) }}</template>
        </el-table-column>
        <template #empty><div class="muted">当前无会话</div></template>
      </el-table>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>采样曲线</h2>
        <span class="spacer" />
        <el-select v-model="selected" multiple collapse-tags class="series-pick"
                   placeholder="选择指标" @change="onSeriesPick">
          <el-option v-for="c in enabledCfg" :key="c.series" :label="c.series" :value="c.series" />
        </el-select>
      </div>
      <div v-if="!enabledCfg.length" class="muted">该设备还没有启用的采样指标（配置功能在后续步骤接入）</div>
      <template v-else>
        <div v-show="hasUnit('percent')" class="chart-box">
          <div class="chart-title">百分比</div>
          <canvas ref="percentEl" />
        </div>
        <div v-show="hasUnit('load')" class="chart-box">
          <div class="chart-title">负载</div>
          <canvas ref="loadEl" />
        </div>
        <div v-show="hasUnit('rate')" class="chart-box">
          <div class="chart-title">速率</div>
          <canvas ref="rateEl" />
        </div>
      </template>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import Chart from 'chart.js/auto'
import { Api } from '~/shared/api.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'
import { ago, bytes, downloadJson, kbPair, time } from '~/shared/utils.js'

const route = useRoute()
const { isPhone, isTablet } = useBreakpoint()
const devid = computed(() => decodeURIComponent(route.params.devid || ''))
const descCols = computed(() => (isPhone.value ? 1 : isTablet.value ? 2 : 4))
const sysCols = computed(() => (isPhone.value ? 1 : 3))

const detail = ref(null)
const sys = ref(null)
const sysTs = ref(0)
const sysErr = ref('')
const loading = ref(false)
const auto = ref(false)
const period = ref(10)

const seriesCfg = ref([])
const enabledCfg = computed(() => seriesCfg.value.filter((c) => c.enabled))
const selected = ref([])
const cache = reactive({})
const lastTs = reactive({})

const percentEl = ref(null)
const loadEl = ref(null)
const rateEl = ref(null)
const charts = {}

const disks = computed(() => {
  const mounts = (sys.value && sys.value.mounts) || []
  return mounts.map((m) => {
    const total = Number(m.total_kb) || 0
    const free = Number(m.free_kb) || 0
    const used = total - free
    const pair = kbPair(used, total)
    return {
      mount: m.mount,
      fstype: m.fstype || '',
      pct: total > 0 ? Math.round((used * 100) / total) : 0,
      used: pair.used,
      total: pair.total,
      unit: pair.unit,
    }
  })
})
const nics = computed(() => (sys.value && sys.value.nics) || [])
const streams = computed(() => (detail.value && detail.value.streams) || [])

function toneOf(p) {
  return p >= 85 ? 'bad' : p >= 70 ? 'warn' : ''
}
function diskColor(p) {
  return p >= 85 ? '#e5534b' : p >= 70 ? '#f5a623' : '#3b82f6'
}

function unitOf(name) {
  if (name === 'cpu' || name === 'mem') return 'percent'
  if (name === 'load') return 'load'
  return 'rate'
}
function hasUnit(u) {
  return selected.value.some((s) => unitOf(s) === u)
}

async function loadAll(opts = {}) {
  const { silent = false, notify = false } = opts
  if (!silent) loading.value = true
  try {
    detail.value = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value))
  } catch (e) {
    if (!silent) ElMessage.error('设备详情加载失败：' + e.message)
  }
  try {
    const r = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/sysinfo')
    sys.value = r.sysinfo
    sysTs.value = r.ts || Date.now()
    sysErr.value = ''
  } catch (e) {
    sysErr.value = '系统信息获取失败：' + e.message
  }
  await pullSeries(!silent)
  if (!silent) loading.value = false
  if (notify) ElMessage.success('已刷新')
}

async function loadSeriesCfg() {
  try {
    const list = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/metrics/series/config')
    seriesCfg.value = Array.isArray(list) ? list : []
  } catch (e) {
    seriesCfg.value = []
  }
  const enabled = enabledCfg.value.map((c) => c.series)
  const prefer = enabled.filter((s) => s === 'cpu' || s === 'mem')
  selected.value = prefer.length ? prefer : enabled.slice(0, 1)
}

function seriesPoints(name) {
  return cache[name] || []
}

function valueArray(name, points) {
  if (unitOf(name) !== 'rate') {
    return points.map((p) => [Number(p.ts), Number(p.v) || 0])
  }
  let prx = 0
  let ptx = 0
  let pts = 0
  const rx = []
  const tx = []
  points.forEach((p) => {
    const t = Number(p.ts) || 0
    const dt = pts ? Math.max(1, t - pts) / 1000 : 1
    rx.push([t, pts ? Math.max(0, (Number(p.rx) - prx) / dt) : 0])
    tx.push([t, pts ? Math.max(0, (Number(p.tx) - ptx) / dt) : 0])
    prx = Number(p.rx) || 0
    ptx = Number(p.tx) || 0
    pts = t
  })
  return { rx, tx }
}

async function pullSeries(full) {
  const names = selected.value.filter(Boolean)
  await Promise.all(names.map(async (name) => {
    const since = full ? 0 : (lastTs[name] || 0)
    try {
      const r = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) +
        '/metrics/series?series=' + encodeURIComponent(name) + '&since_ts=' + since)
      if (r.truncated && since) {
        lastTs[name] = 0
        return pullSeries(true)
      }
      if (!since) cache[name] = []
      cache[name] = (cache[name] || []).concat(r.points || [])
      while (cache[name].length > 300) cache[name].shift()
      lastTs[name] = r.newest_ts || since
    } catch (e) { /* 离线或该指标无数据 */ }
  }))
  await nextTick()
  renderCharts()
}

const palette = ['#4a9eff', '#f5a623', '#57ab5a', '#b083f0', '#e5534b']

function renderCharts() {
  const groups = { percent: [], load: [], rate: [] }
  selected.value.forEach((s) => groups[unitOf(s)].push(s))
  const els = { percent: percentEl.value, load: loadEl.value, rate: rateEl.value }
  for (const unit of ['percent', 'load', 'rate']) {
    const el = els[unit]
    if (!el) continue
    if (charts[unit]) {
      charts[unit].destroy()
      delete charts[unit]
    }
    if (!groups[unit].length) continue
    const tsSet = new Set()
    const data = {}
    groups[unit].forEach((name) => {
      const v = valueArray(name, seriesPoints(name))
      data[name] = v
      if (Array.isArray(v)) v.forEach((x) => tsSet.add(x[0]))
      else {
        v.rx.forEach((x) => tsSet.add(x[0]))
        v.tx.forEach((x) => tsSet.add(x[0]))
      }
    })
    const labels = [...tsSet].sort((a, b) => a - b)
    const idx = new Map(labels.map((t, i) => [t, i]))
    const put = (arr) => {
      const out = new Array(labels.length).fill(null)
      arr.forEach((x) => {
        const i = idx.get(x[0])
        if (i !== undefined) out[i] = x[1]
      })
      return out
    }
    const datasets = []
    groups[unit].forEach((name, si) => {
      const color = palette[si % palette.length]
      const v = data[name]
      if (Array.isArray(v)) {
        datasets.push({
          label: name, data: put(v), borderColor: color, backgroundColor: color + '1f',
          borderWidth: 2, pointRadius: 0, fill: true, tension: 0.2,
        })
      } else {
        datasets.push({ label: name + ' RX', data: put(v.rx), borderColor: color, borderWidth: 2, pointRadius: 0 })
        datasets.push({
          label: name + ' TX', data: put(v.tx), borderColor: color, borderDash: [4, 3],
          borderWidth: 2, pointRadius: 0,
        })
      }
    })
    charts[unit] = new Chart(el.getContext('2d'), {
      type: 'line',
      data: {
        labels: labels.map((t) => new Date(t).toLocaleTimeString('zh-CN', { hour12: false })),
        datasets,
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: { legend: { display: true, labels: { boxWidth: 10 } } },
        scales: { x: { display: false }, y: { beginAtZero: true, ticks: { maxTicksLimit: 4 } } },
      },
    })
  }
}

async function onSeriesPick() {
  await pullSeries(true)
}

function onAutoChange() {
  stopTimer()
  if (auto.value) timer = setInterval(() => loadAll({ silent: true }), Math.max(5, Number(period.value) || 10) * 1000)
}

function exportJson() {
  downloadJson(devid.value + '-detail.json', {
    detail: detail.value,
    sysinfo: sys.value,
    sysinfo_ts: sysTs.value,
    series: seriesCfg.value,
  })
}

let timer = null
function stopTimer() {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}

async function init() {
  stopTimer()
  auto.value = false
  detail.value = null
  sys.value = null
  sysErr.value = ''
  seriesCfg.value = []
  selected.value = []
  Object.keys(cache).forEach((k) => delete cache[k])
  Object.keys(lastTs).forEach((k) => delete lastTs[k])
  await loadSeriesCfg()
  await loadAll()
}

watch(() => route.params.devid, (v) => { if (v) init() })
onMounted(init)
onBeforeUnmount(() => {
  stopTimer()
  Object.values(charts).forEach((c) => { try { c.destroy() } catch (e) { /* 已销毁 */ } })
})
</script>

<style scoped>
.period { width: 84px; }
:deep(.el-table .cell) { white-space: nowrap; }
.disk { padding: 8px 0; border-bottom: 1px solid var(--el-border-color-lighter); }
.disk:last-child { border-bottom: 0; }
.disk-top { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.disk-top .mount { font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.fs {
  font-size: 11px;
  color: var(--el-text-color-secondary);
  background: var(--el-fill-color);
  border-radius: 4px;
  padding: 0 6px;
}
.disk-top .pct { margin-left: auto; font-size: 12px; color: var(--el-text-color-secondary); font-variant-numeric: tabular-nums; }
.disk-top .pct.warn { color: #c98a12; }
.disk-top .pct.bad { color: #d9534f; }
.size { margin-top: 3px; font-size: 11px; color: var(--el-text-color-secondary); }
.chart-box { margin-top: 8px; height: 190px; position: relative; }
.chart-title { font-size: 12px; color: var(--el-text-color-secondary); margin-bottom: 4px; }
.series-pick { width: 260px; }
@media (max-width: 599px) {
  .period { width: 76px; }
  .series-pick { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
