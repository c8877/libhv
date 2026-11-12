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
        <h2>能力配置</h2>
        <span class="spacer" />
        <el-button v-if="sys && canEdit" size="small" @click="toggleCaps">{{ capsOpen ? '收起' : '编辑' }}</el-button>
      </div>
      <div v-if="sys" class="kv-rows">
        <div class="kv-row">
          <span class="k">agent 版本</span>
          <span class="v mono">{{ sys.version || '—' }}</span>
        </div>
        <div class="kv-row">
          <span class="k">能力</span>
          <span class="v chips">
            <el-tag v-for="c in capsChips" :key="c" size="small" effect="plain">{{ c }}</el-tag>
            <span v-if="!capsChips.length" class="muted">—</span>
          </span>
        </div>
        <div class="kv-row">
          <span class="k">启动参数</span>
          <span class="v"><code class="args">{{ sys.args || '—' }}</code></span>
        </div>
      </div>
      <el-skeleton v-else :rows="3" animated />
      <div v-if="capsOpen && sys" class="caps-edit">
        <div class="form-grid">
          <label>转发端口<el-input v-model="capsForm.ports" size="small" placeholder="22, 8080, 8099" /></label>
          <label>文件目录<el-input v-model="capsForm.file_dir" size="small" placeholder="/opt/data/files" /></label>
          <label class="check">
            <el-checkbox v-model="capsForm.shell" :disabled="!isAdmin">允许 Shell</el-checkbox>
          </label>
        </div>
        <div class="form-actions">
          <span v-if="!isAdmin" class="muted">仅 admin 可修改 Shell</span>
          <span class="spacer" />
          <el-button size="small" @click="capsOpen = false">取消</el-button>
          <el-button size="small" type="primary" :loading="capsSaving" @click="saveCaps">保存并下发</el-button>
        </div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head"><h2>证书</h2></div>
      <el-descriptions v-if="sys && sys.mtls" :column="descCols" size="small" border>
        <el-descriptions-item label="证书主体"><span class="mono">{{ sys.cert_subject || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="签发者"><span class="mono">{{ sys.cert_issuer || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="序列号"><span class="mono">{{ sys.cert_serial || '—' }}</span></el-descriptions-item>
        <el-descriptions-item label="状态">
          <el-tag :type="sys.cert_valid ? 'success' : 'danger'" size="small">
            {{ sys.cert_valid ? '有效' : '已过期' }}
          </el-tag>
        </el-descriptions-item>
      </el-descriptions>
      <div v-else-if="sys" class="muted">未启用 mTLS</div>
      <el-skeleton v-else :rows="2" animated />
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>采样曲线</h2>
        <span class="spacer" />
        <el-switch v-model="sampleAuto" active-text="自动采集" @change="onSampleAuto" />
        <el-button size="small" @click="selectAllSeries">全选</el-button>
        <el-button size="small" @click="clearSeries">清空</el-button>
        <el-button size="small" @click="pullSeries(true)">全量</el-button>
        <el-select v-model="selected" multiple collapse-tags class="series-pick"
                   placeholder="选择指标" @change="onSeriesPick">
          <el-option v-for="c in enabledCfg" :key="c.series" :label="c.series" :value="c.series" />
        </el-select>
      </div>
      <div v-if="!enabledCfg.length" class="muted">该设备还没有启用的采样指标，可在下方「采样配置」中添加</div>
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

    <div class="panel">
      <div class="panel-head">
        <h2>采样配置</h2>
        <span class="muted">{{ seriesCfg.length }} 项 · {{ unsyncedCount }} 项未同步</span>
        <span class="spacer" />
        <el-button v-if="canEdit" size="small" @click="openAddMetric">+ 添加指标</el-button>
      </div>
      <el-table ref="metricTable" :data="seriesCfg" size="small" row-key="series" @row-click="onMetricRow">
        <el-table-column type="expand">
          <template #default="{ row }">
            <div class="metric-edit" @click.stop>
              <div class="form-grid">
                <label>间隔（秒）<el-input-number v-model="row.interval_sec" :min="1" :max="300" size="small" /></label>
                <label>保留点数<el-input-number v-model="row.points" :min="60" :max="86400" size="small" /></label>
                <div class="readonly">预计保留 <b>{{ dur(row) }}</b></div>
              </div>
              <div class="form-actions">
                <span class="muted">{{ metricHint(row) }}</span>
                <span class="spacer" />
                <el-button v-if="canEdit" size="small" type="danger" plain @click="deleteMetric(row)">删除</el-button>
                <el-button size="small" @click="cancelMetric">取消</el-button>
                <el-button v-if="canEdit" size="small" type="primary" @click="saveMetricRow(row)">保存并下发</el-button>
              </div>
            </div>
          </template>
        </el-table-column>
        <el-table-column label="指标" min-width="150">
          <template #default="{ row }">
            <b>{{ row.series }}</b><span class="sub">{{ unitLabel(row.series) }}</span>
          </template>
        </el-table-column>
        <el-table-column label="间隔" width="90">
          <template #default="{ row }"><span class="num">{{ row.interval_sec }} s</span></template>
        </el-table-column>
        <el-table-column label="保留" min-width="130">
          <template #default="{ row }">
            <span class="num">{{ row.points }} 点</span><span class="sub">· {{ dur(row) }}</span>
          </template>
        </el-table-column>
        <el-table-column label="同步" width="110">
          <template #default="{ row }">
            <span class="sync" :class="row.enabled ? (row.synced ? 'ok' : 'warn') : 'off'">
              {{ row.enabled ? (row.synced ? '已同步' : '未同步') : '禁用' }}
            </span>
          </template>
        </el-table-column>
        <el-table-column label="启用" width="80">
          <template #default="{ row }">
            <el-switch :model-value="row.enabled" :disabled="!canEdit" @click.stop @change="(v) => toggleMetric(row, v)" />
          </template>
        </el-table-column>
        <template #empty><div class="muted">暂无配置</div></template>
      </el-table>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>串口 TTY</h2>
        <span class="spacer" />
        <el-input v-model="ttyQuery" size="small" class="tty-search" placeholder="搜索别名 / 路径" clearable />
        <el-button size="small" @click="loadTtys(true)">刷新</el-button>
        <el-button v-if="canEdit" size="small" @click="manualAddTty">手动添加</el-button>
      </div>
      <el-tabs v-model="ttyTab">
        <el-tab-pane :label="'已配置 (' + ttys.length + ')'" name="config">
          <el-table :data="filteredTtys" size="small" row-key="path">
            <el-table-column label="设备" min-width="180">
              <template #default="{ row }">
                <b>{{ row.alias || '—' }}</b>
                <div class="path" :title="row.path">{{ row.path }}</div>
              </template>
            </el-table-column>
            <el-table-column label="状态" width="90">
              <template #default="{ row }">
                <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '禁用' }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="参数" min-width="120">
              <template #default="{ row }">{{ row.baud }} · {{ row.data_bits }}{{ row.parity }}{{ row.stop_bits }}</template>
            </el-table-column>
            <el-table-column label="RX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyRx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column label="TX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyTx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column label="操作" :width="canEdit ? 190 : 90" fixed="right">
              <template #default="{ row }">
                <el-button size="small" type="primary" link @click="openTtyConsole(row)">调试</el-button>
                <el-button v-if="canEdit" size="small" link @click="openTtyDialog(row, false)">编辑</el-button>
                <el-button v-if="canEdit" size="small" type="danger" link @click="deleteTty(row)">删除</el-button>
              </template>
            </el-table-column>
            <template #empty><div class="muted">暂无已配置串口</div></template>
          </el-table>
        </el-tab-pane>
        <el-tab-pane :label="'未配置 (' + unconfigured.length + ')'" name="unconfig">
          <el-table :data="filteredUnconfigured" size="small" row-key="path">
            <el-table-column label="设备" min-width="180">
              <template #default="{ row }">
                <b>未配置</b>
                <div class="path" :title="row.path">{{ row.path }}</div>
              </template>
            </el-table-column>
            <el-table-column label="状态" width="90"><span class="muted">未配置</span></el-table-column>
            <el-table-column label="参数" min-width="120"><span class="muted">—</span></el-table-column>
            <el-table-column label="RX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyRx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column label="TX" width="90">
              <template #default="{ row }"><span class="num">{{ ttyTx(row.path) }}</span></template>
            </el-table-column>
            <el-table-column label="操作" width="100" fixed="right">
              <template #default="{ row }">
                <el-button v-if="canEdit" size="small" type="primary" link @click="openTtyDialog(row, true)">添加</el-button>
                <span v-else class="muted">—</span>
              </template>
            </el-table-column>
            <template #empty><div class="muted">{{ ttyQuery ? '没有匹配的串口' : '没有未配置的串口' }}</div></template>
          </el-table>
        </el-tab-pane>
      </el-tabs>
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

    <el-dialog v-model="addMetricOpen" title="添加采样指标" width="420px">
      <el-checkbox-group v-model="addMetricPick" class="cand-list">
        <el-checkbox v-for="c in addCandidates" :key="c" :value="c">{{ c }}</el-checkbox>
      </el-checkbox-group>
      <div v-if="!addCandidates.length" class="muted">没有可添加的指标</div>
      <template #footer>
        <el-button @click="addMetricOpen = false">取消</el-button>
        <el-button type="primary" :disabled="!addMetricPick.length" @click="addMetrics">添加</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="ttyDialog" :title="ttyEditing.isNew ? '添加串口' : '编辑串口'" width="480px">
      <div class="tty-form">
        <label>别名<el-input v-model="ttyEditing.alias" placeholder="设备内唯一，可留空" /></label>
        <label>路径<el-input v-model="ttyEditing.path" :disabled="!ttyEditing.isNew" placeholder="/dev/ttyS0" /></label>
        <label>波特率
          <el-select v-model="ttyEditing.baud">
            <el-option v-for="b in bauds" :key="b" :label="b" :value="b" />
          </el-select>
        </label>
        <label>数据位
          <el-select v-model="ttyEditing.data_bits">
            <el-option v-for="b in [5, 6, 7, 8]" :key="b" :label="b" :value="b" />
          </el-select>
        </label>
        <label>校验
          <el-select v-model="ttyEditing.parity">
            <el-option label="无 (N)" value="N" />
            <el-option label="偶 (E)" value="E" />
            <el-option label="奇 (O)" value="O" />
          </el-select>
        </label>
        <label>停止位
          <el-select v-model="ttyEditing.stop_bits">
            <el-option :label="1" :value="1" />
            <el-option :label="2" :value="2" />
          </el-select>
        </label>
        <label class="check"><el-checkbox v-model="ttyEditing.enabled">启用</el-checkbox></label>
      </div>
      <div class="tty-form-extra">
        <el-button v-if="!ttyEditing.isNew" size="small" @click="copyPath(ttyEditing.path)">复制路径</el-button>
        <el-button v-if="!ttyEditing.isNew && isAdmin" size="small" @click="takeoverTty(ttyEditing)">强制接管</el-button>
      </div>
      <template #footer>
        <el-button @click="ttyDialog = false">取消</el-button>
        <el-button type="primary" :loading="ttySaving" @click="saveTty">保存并下发</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import Chart from 'chart.js/auto'
import { Api } from '@/shared/api.js'
import { canManage, roleOf } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { ago, bytes, downloadJson, kbPair, time } from '@/shared/utils.js'

const route = useRoute()
const router = useRouter()
const { isPhone, isTablet } = useBreakpoint()
const devid = computed(() => decodeURIComponent(route.params.devid || ''))
const descCols = computed(() => (isPhone.value ? 1 : isTablet.value ? 2 : 4))
const sysCols = computed(() => (isPhone.value ? 1 : 3))
const canEdit = computed(() => canManage())
const isAdmin = computed(() => roleOf() === 'admin')

const detail = ref(null)
const sys = ref(null)
const sysTs = ref(0)
const sysErr = ref('')
const loading = ref(false)
const auto = ref(false)
const period = ref(10)
let sysFull = false
let sysRetry = null

const seriesCfg = ref([])
const enabledCfg = computed(() => seriesCfg.value.filter((c) => c.enabled))
const unsyncedCount = computed(() => seriesCfg.value.filter((c) => c.enabled && !c.synced).length)
const selected = ref([])
const cache = reactive({})
const lastTs = reactive({})
const pullAt = reactive({})
const sampleAuto = ref(false)
let sampleTimer = null
const metricTable = ref(null)

const percentEl = ref(null)
const loadEl = ref(null)
const rateEl = ref(null)
const charts = {}

const capsOpen = ref(false)
const capsSaving = ref(false)
const capsForm = reactive({ ports: '', file_dir: '', shell: false })

const addMetricOpen = ref(false)
const addMetricPick = ref([])

const ttys = ref([])
const ttyDisc = ref([])
const ttyLoaded = ref(false)
const ttyTab = ref('config')
const ttyQuery = ref('')
const ttyDialog = ref(false)
const ttySaving = ref(false)
const bauds = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
const ttyEditing = reactive({
  isNew: true, path: '', alias: '', baud: 115200, data_bits: 8,
  parity: 'N', stop_bits: 1, enabled: true, roles: 'operator,manager,admin',
})

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
const caps = computed(() => (sys.value && sys.value.caps) || {})
const capsChips = computed(() => {
  const c = caps.value
  const out = []
  ;(c.ports || []).forEach((p) => out.push('TCP ' + p))
  if (c.shell) out.push('Shell')
  if (c.sysinfo) out.push('SYSINFO')
  if (c.file_dir) out.push('文件 ' + c.file_dir)
  out.push(c.mtls ? 'mTLS' : 'mTLS 未启用')
  return out
})

const ttySerial = computed(() => {
  const map = {}
  ;((sys.value && sys.value.serial) || []).forEach((x) => { map[x.path] = x })
  return map
})
const configuredPaths = computed(() => new Set(ttys.value.map((t) => t.path)))
const unconfigured = computed(() => ttyDisc.value.filter((d) => !configuredPaths.value.has(d.path)))
const ttyQ = computed(() => ttyQuery.value.trim().toLowerCase())
const filteredTtys = computed(() => ttys.value.filter((t) => {
  if (!ttyQ.value) return true
  return (t.alias || '').toLowerCase().includes(ttyQ.value) || (t.path || '').toLowerCase().includes(ttyQ.value)
}))
const filteredUnconfigured = computed(() => unconfigured.value.filter((d) => {
  if (!ttyQ.value) return true
  return (d.path || '').toLowerCase().includes(ttyQ.value)
}))
const addCandidates = computed(() => {
  const existing = new Set(seriesCfg.value.map((c) => c.series))
  const out = ['cpu', 'mem', 'load']
  ;((sys.value && sys.value.nics) || []).forEach((n) => { if (n.name) out.push('net:' + n.name) })
  const paths = new Set()
  ;((sys.value && sys.value.serial) || []).forEach((s) => paths.add(s.path))
  ttys.value.forEach((t) => paths.add(t.path))
  ttyDisc.value.forEach((d) => paths.add(d.path))
  paths.forEach((p) => {
    const b = String(p).split('/').pop()
    if (b && b.startsWith('tty')) out.push('serial:' + b)
  })
  return [...new Set(out)].filter((x) => !existing.has(x))
})

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
function unitLabel(name) {
  return { percent: '百分比', load: '负载', rate: '速率' }[unitOf(name)]
}
function hasUnit(u) {
  return selected.value.some((s) => unitOf(s) === u)
}
function dur(c) {
  const sec = (Number(c.interval_sec) || 1) * (Number(c.points) || 0)
  if (sec >= 3600) return (sec / 3600).toFixed(sec % 3600 ? 1 : 0) + ' h'
  if (sec >= 60) return Math.round(sec / 60) + ' m'
  return sec + ' s'
}
function metricHint(row) {
  if (!row.enabled) return '已禁用，不参与采样'
  return row.synced ? '已下发并确认' : '已下发，等待 agent 确认'
}

function mergeSysinfo(p) {
  if (!p || !sys.value) return
  if (p.swap_free_kb != null) sys.value.swap_free_kb = p.swap_free_kb
  ;(p.mounts || []).forEach((pm) => {
    const m = (sys.value.mounts || []).find((x) => x.mount === pm.mount)
    if (m && pm.free_kb != null) m.free_kb = pm.free_kb
  })
  ;(p.nics || []).forEach((pn) => {
    const n = (sys.value.nics || []).find((x) => x.name === pn.name)
    if (!n) return
    if (pn.ip != null) n.ip = pn.ip
    if (pn.up != null) n.up = pn.up
  })
  if (p.serial) sys.value.serial = p.serial
}

async function loadSysinfo(silent) {
  try {
    const mode = sysFull ? 'inc' : 'full'
    const r = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/sysinfo?mode=' + mode)
    if (r.mode === 'inc') mergeSysinfo(r.sysinfo)
    else {
      sys.value = r.sysinfo
      sysFull = true
    }
    sysTs.value = r.ts || Date.now()
    sysErr.value = ''
    if (sys.value && !sys.value.cpu_count && !sysRetry) {
      sysRetry = setTimeout(() => { sysRetry = null; loadSysinfo(true) }, 1200)
    }
  } catch (e) {
    if (!silent) sysErr.value = '系统信息获取失败：' + e.message
  }
}

async function loadAll(opts = {}) {
  const { silent = false, notify = false } = opts
  if (!silent) loading.value = true
  try {
    detail.value = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value))
  } catch (e) {
    if (!silent) ElMessage.error('设备详情加载失败：' + e.message)
  }
  await loadSysinfo(silent)
  await pullSeries(!silent)
  if (!silent) loading.value = false
  if (notify) ElMessage.success('已刷新')
}

async function fetchSeriesCfg() {
  try {
    const list = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/metrics/series/config')
    seriesCfg.value = Array.isArray(list) ? list : []
  } catch (e) {
    seriesCfg.value = []
  }
}

function pickSeries() {
  const enabled = enabledCfg.value.map((c) => c.series)
  const prefer = enabled.filter((s) => s === 'cpu' || s === 'mem')
  selected.value = prefer.length ? prefer : enabled.slice(0, 1)
}

function syncSeriesSelection() {
  const enabled = enabledCfg.value.map((c) => c.series)
  selected.value = selected.value.filter((s) => enabled.includes(s))
  if (!selected.value.length) pickSeries()
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

async function pullSeriesNames(names, full) {
  await Promise.all(names.map(async (name) => {
    const since = full ? 0 : (lastTs[name] || 0)
    try {
      const r = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) +
        '/metrics/series?series=' + encodeURIComponent(name) + '&since_ts=' + since)
      if (r.truncated && since) {
        lastTs[name] = 0
        return pullSeriesNames([name], true)
      }
      if (!since) cache[name] = []
      cache[name] = (cache[name] || []).concat(r.points || [])
      while (cache[name].length > 300) cache[name].shift()
      lastTs[name] = r.newest_ts || since
    } catch (e) { /* 离线或该指标无数据 */ }
  }))
}

async function pullSeries(full) {
  const names = selected.value.filter(Boolean)
  if (!names.length) {
    renderCharts()
    return
  }
  await pullSeriesNames(names, full)
  const now = Date.now()
  names.forEach((n) => { pullAt[n] = now })
  await nextTick()
  renderCharts()
}

async function onSeriesPick() {
  await pullSeries(true)
}

function selectAllSeries() {
  selected.value = enabledCfg.value.map((c) => c.series)
  pullSeries(true)
}
function clearSeries() {
  selected.value = []
  renderCharts()
}

function onAutoChange() {
  stopTimer()
  if (auto.value) timer = setInterval(() => loadAll({ silent: true }), Math.max(5, Number(period.value) || 10) * 1000)
}

function onSampleAuto() {
  stopSample()
  if (sampleAuto.value) sampleTimer = setInterval(sampleTick, 1000)
}

async function sampleTick() {
  if (!sampleAuto.value || document.hidden) return
  const now = Date.now()
  const due = selected.value.filter((name) => {
    const cfg = seriesCfg.value.find((c) => c.series === name)
    if (!cfg) return false
    return now - (pullAt[name] || 0) >= Math.max(1, Number(cfg.interval_sec) || 5) * 1000
  })
  if (!due.length) return
  due.forEach((n) => { pullAt[n] = now })
  await pullSeriesNames(due, false)
  renderCharts()
}

function stopSample() {
  if (sampleTimer) {
    clearInterval(sampleTimer)
    sampleTimer = null
  }
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

function toggleCaps() {
  capsOpen.value = !capsOpen.value
  if (capsOpen.value && sys.value) {
    const c = sys.value.caps || {}
    capsForm.ports = (c.ports || []).join(', ')
    capsForm.file_dir = c.file_dir || ''
    capsForm.shell = !!c.shell
  }
}

async function saveCaps() {
  const raw = capsForm.ports.trim()
  const ports = raw ? raw.split(/[,\s]+/).filter(Boolean).map(Number) : []
  if (ports.some((x) => !Number.isInteger(x) || x <= 0 || x > 65535)) {
    ElMessage.error('端口必须是 1–65535 的整数')
    return
  }
  const body = { ports }
  const dir = capsForm.file_dir.trim()
  if (dir) body.file_dir = dir
  if (isAdmin.value) body.shell = capsForm.shell
  capsSaving.value = true
  try {
    const r = await Api.put('/api/v1/agents/' + encodeURIComponent(devid.value) + '/caps', body)
    capsOpen.value = false
    if (r && r.persisted === false) ElMessage.warning('已生效，但未能写入配置文件，重启后会丢失')
    else ElMessage.success('能力已更新')
    await loadAll({ silent: true })
  } catch (e) {
    ElMessage.error('失败：' + e.message)
  } finally {
    capsSaving.value = false
  }
}

function onMetricRow(row) {
  metricTable.value && metricTable.value.toggleRowExpansion(row)
}

async function saveMetricList(list) {
  try {
    await Api.put('/api/v1/agents/' + encodeURIComponent(devid.value) + '/metrics/series/config',
      list.map((c) => ({
        series: c.series,
        enabled: !!c.enabled,
        interval_sec: Number(c.interval_sec) || 5,
        points: Number(c.points) || 720,
      })))
    await fetchSeriesCfg()
    syncSeriesSelection()
    await pullSeries(true)
    ElMessage.success('采样配置已下发')
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  }
}

async function saveMetricRow(row) {
  await saveMetricList(seriesCfg.value.map((c) => (c.series === row.series ? row : c)))
}

async function toggleMetric(row, v) {
  row.enabled = v
  await saveMetricList(seriesCfg.value.map((c) => (c.series === row.series ? Object.assign({}, c, { enabled: v }) : c)))
}

async function deleteMetric(row) {
  try {
    await ElMessageBox.confirm('确定删除指标 ' + row.series + '？', '删除指标', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) { return }
  await saveMetricList(seriesCfg.value.filter((c) => c.series !== row.series))
}

async function cancelMetric() {
  await fetchSeriesCfg()
  syncSeriesSelection()
}

function openAddMetric() {
  addMetricPick.value = []
  addMetricOpen.value = true
}

async function addMetrics() {
  const picked = addMetricPick.value
  if (!picked.length) return
  const list = seriesCfg.value.concat(picked.map((s) => ({ series: s, enabled: true, interval_sec: 5, points: 720 })))
  addMetricOpen.value = false
  await saveMetricList(list)
}

function ttyRx(path) {
  const x = ttySerial.value[path]
  return x ? bytes(x.rx) : '—'
}
function ttyTx(path) {
  const x = ttySerial.value[path]
  return x ? bytes(x.tx) : '—'
}

async function loadTtys(force) {
  if (ttyLoaded.value && !force) return
  try {
    const list = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/ttys')
    ttys.value = Array.isArray(list) ? list : []
  } catch (e) {
    ttys.value = []
  }
  if (canEdit.value) {
    try {
      const d = await Api.get('/api/v1/agents/' + encodeURIComponent(devid.value) + '/ttys/discovered')
      ttyDisc.value = Array.isArray(d) ? d : []
    } catch (e) {
      ttyDisc.value = []
    }
  } else {
    ttyDisc.value = []
  }
  ttyLoaded.value = true
}

function openTtyDialog(row, isNew) {
  ttyEditing.isNew = !!isNew
  ttyEditing.path = row.path || ''
  ttyEditing.alias = isNew ? '' : (row.alias || '')
  ttyEditing.baud = isNew ? 115200 : (Number(row.baud) || 115200)
  ttyEditing.data_bits = isNew ? 8 : (Number(row.data_bits) || 8)
  ttyEditing.parity = isNew ? 'N' : (row.parity || 'N')
  ttyEditing.stop_bits = isNew ? 1 : (Number(row.stop_bits) || 1)
  ttyEditing.enabled = isNew ? true : !!row.enabled
  ttyEditing.roles = (row.roles || 'operator,manager,admin')
  ttyDialog.value = true
}

function manualAddTty() {
  openTtyDialog({ path: '' }, true)
}

async function saveTty() {
  const path = ttyEditing.path.trim()
  const alias = ttyEditing.alias.trim()
  if (!path || path[0] !== '/') {
    ElMessage.error('路径必须以 / 开头')
    return
  }
  if (alias && !/^[A-Za-z0-9_-]{1,32}$/.test(alias)) {
    ElMessage.error('别名只能包含字母、数字、下划线和短横线')
    return
  }
  ttySaving.value = true
  try {
    await Api.put('/api/v1/agents/' + encodeURIComponent(devid.value) + '/ttys', {
      path,
      alias,
      enabled: ttyEditing.enabled,
      baud: ttyEditing.baud,
      data_bits: ttyEditing.data_bits,
      parity: ttyEditing.parity,
      stop_bits: ttyEditing.stop_bits,
      roles: ttyEditing.roles,
    })
    ttyDialog.value = false
    await loadTtys(true)
    ElMessage.success('串口配置已下发')
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    ttySaving.value = false
  }
}

async function deleteTty(row) {
  try {
    await ElMessageBox.confirm('确定删除 ' + (row.alias || row.path) + ' 的串口配置？', '删除串口配置', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/agents/' + encodeURIComponent(devid.value) + '/ttys?path=' + encodeURIComponent(row.path))
    await loadTtys(true)
    ElMessage.success('已删除')
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

async function copyPath(p) {
  try {
    await navigator.clipboard.writeText(p)
    ElMessage.success('路径已复制')
  } catch (e) {
    ElMessage.warning('复制失败：' + p)
  }
}

async function takeoverTty(row) {
  const name = row.alias || row.path
  try {
    const r = await ElMessageBox.prompt('会断开当前会话，输入别名确认：' + name, '强制接管', {
      confirmButtonText: '接管', cancelButtonText: '取消',
    })
    if (String(r.value || '') !== name) {
      ElMessage.warning('输入不一致，已取消')
      return
    }
  } catch (e) { return }
  router.push({
    path: '/tty/' + encodeURIComponent(devid.value) + '/' + encodeURIComponent(name),
    query: { takeover: '1' },
  })
}

function openTtyConsole(row) {
  const name = row.alias || row.path
  router.push('/tty/' + encodeURIComponent(devid.value) + '/' + encodeURIComponent(name))
}

function exportJson() {
  downloadJson(devid.value + '-detail.json', {
    detail: detail.value,
    sysinfo: sys.value,
    sysinfo_ts: sysTs.value,
    series: seriesCfg.value,
    ttys: ttys.value,
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
  stopSample()
  auto.value = false
  sampleAuto.value = false
  detail.value = null
  sys.value = null
  sysErr.value = ''
  sysFull = false
  seriesCfg.value = []
  selected.value = []
  ttys.value = []
  ttyDisc.value = []
  ttyLoaded.value = false
  ttyTab.value = 'config'
  addMetricOpen.value = false
  capsOpen.value = false
  if (sysRetry) { clearTimeout(sysRetry); sysRetry = null }
  Object.keys(cache).forEach((k) => delete cache[k])
  Object.keys(lastTs).forEach((k) => delete lastTs[k])
  Object.keys(pullAt).forEach((k) => delete pullAt[k])
  await fetchSeriesCfg()
  pickSeries()
  await loadAll()
  loadTtys()
}

watch(() => route.params.devid, (v) => { if (v) init() })
onMounted(init)
onBeforeUnmount(() => {
  stopTimer()
  stopSample()
  if (sysRetry) { clearTimeout(sysRetry); sysRetry = null }
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
.series-pick { width: 230px; }

.kv-rows { border: 1px solid var(--el-border-color-lighter); border-radius: 8px; overflow: hidden; }
.kv-row { display: flex; align-items: flex-start; gap: 12px; padding: 9px 12px; border-bottom: 1px solid var(--el-border-color-lighter); }
.kv-row:last-child { border-bottom: 0; }
.kv-row .k { flex: none; width: 92px; color: var(--el-text-color-secondary); font-size: 12px; line-height: 24px; }
.kv-row .v { min-width: 0; font-size: 13px; word-break: break-all; }
.kv-row .chips { display: flex; flex-wrap: wrap; gap: 6px; }
.args { font-size: 12px; color: var(--el-text-color-regular); white-space: pre-wrap; word-break: break-all; }
.caps-edit {
  margin-top: 12px;
  padding: 12px;
  background: var(--el-fill-color-lighter);
  border-radius: 8px;
}
.form-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
  gap: 10px 14px;
  align-items: end;
}
.form-grid > label { display: flex; flex-direction: column; gap: 5px; font-size: 12px; color: var(--el-text-color-secondary); }
.form-grid > label.check { flex-direction: row; align-items: center; }
.form-grid .readonly { font-size: 12px; color: var(--el-text-color-secondary); }
.form-actions { display: flex; align-items: center; gap: 8px; margin-top: 12px; }
.form-actions .spacer { flex: 1; }

.sub { margin-left: 6px; font-size: 11px; color: var(--el-text-color-secondary); }
.num { font-variant-numeric: tabular-nums; }
.sync { font-size: 12px; }
.sync::before {
  content: '';
  display: inline-block;
  width: 6px;
  height: 6px;
  border-radius: 50%;
  margin-right: 6px;
  vertical-align: 1px;
  background: var(--el-text-color-placeholder);
}
.sync.ok::before { background: var(--el-color-success); }
.sync.warn::before { background: var(--el-color-warning); }
.sync.off::before { background: var(--el-text-color-placeholder); }
.metric-edit { padding: 6px 12px 12px 42px; }

.tty-search { width: 190px; }
.path { font-size: 11px; color: var(--el-text-color-secondary); overflow: hidden; text-overflow: ellipsis; }
.cand-list { display: flex; flex-direction: column; gap: 4px; }
.tty-form { display: grid; grid-template-columns: 1fr 1fr; gap: 12px 16px; }
.tty-form > label { display: flex; flex-direction: column; gap: 5px; font-size: 12px; color: var(--el-text-color-secondary); }
.tty-form > label.check { flex-direction: row; align-items: center; }
.tty-form-extra { display: flex; gap: 8px; margin-top: 12px; }

@media (max-width: 599px) {
  .period { width: 76px; }
  .series-pick { width: 100%; }
  .panel-head { flex-wrap: wrap; }
  .tty-search { width: 100%; }
  .tty-form { grid-template-columns: 1fr; }
  .metric-edit { padding-left: 12px; }
}
</style>
