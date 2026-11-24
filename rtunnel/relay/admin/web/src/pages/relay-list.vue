<template>
  <div class="panel">
    <div class="panel-head">
      <h2>中继列表</h2>
      <span class="muted sum">
        下发列表 {{ order.length }}/8 · rev {{ rev || '—' }} ·
        {{ configured ? '已启用' : '未启用' }}
      </span>
      <el-tag v-if="dirty" type="warning" size="small">未保存</el-tag>
      <span class="spacer" />
      <el-button @click="reloadAll">刷新</el-button>
      <el-button type="primary" :disabled="!dirty" @click="saveList">保存列表</el-button>
    </div>

    <el-tabs v-model="tab" @tab-change="onTab">
      <el-tab-pane label="中继地址" name="dict">
        <div class="bar">
          <el-input v-model="q" placeholder="搜索地址 / 名称" clearable class="w220" @keyup.enter="loadDict">
            <template #prefix><el-icon><Search /></el-icon></template>
          </el-input>
          <el-select v-model="stateFilter" class="w130" placeholder="全部状态">
            <el-option label="全部" value="all" />
            <el-option label="已加入" value="in" />
            <el-option label="未加入" value="out" />
            <el-option label="已停用" value="off" />
          </el-select>
          <el-button link type="primary" @click="loadDict">查询</el-button>
          <span class="spacer" />
          <el-button type="primary" @click="openAdd">新增地址</el-button>
          <el-button @click="openImport">批量导入</el-button>
        </div>

        <el-table :data="dictRows" v-loading="loading" @selection-change="onDictSelect">
          <el-table-column type="selection" width="44" />
          <el-table-column prop="host" label="地址" min-width="180">
            <template #default="{ row }">
              <span class="mono">{{ row.host }}</span>
              <div v-if="row.description" class="muted">{{ row.description }}</div>
            </template>
          </el-table-column>
          <el-table-column prop="port" label="端口" width="90" />
          <el-table-column prop="name" label="名称" min-width="110">
            <template #default="{ row }">{{ row.name || '—' }}</template>
          </el-table-column>
          <el-table-column label="状态" width="90">
            <template #default="{ row }">
              <el-tag :type="row.enabled ? 'success' : 'info'" size="small">
                {{ row.enabled ? '启用' : '停用' }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column label="下发顺序" width="170">
            <template #default="{ row }">
              <template v-if="posOf(row.id)">
                <el-tag size="small" effect="plain" class="pos">{{ posOf(row.id) }}</el-tag>
                <el-button link :disabled="posOf(row.id) === 1" @click="move(row.id, -1)">↑</el-button>
                <el-button link :disabled="posOf(row.id) === order.length" @click="move(row.id, 1)">↓</el-button>
                <el-button link type="danger" @click="removeFromList(row.id)">移出</el-button>
              </template>
              <span v-else class="muted">—</span>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="120">
            <template #default="{ row }">
              <el-button link type="primary" @click="openEdit(row)">编辑</el-button>
              <el-button link type="danger" @click="removeDict(row)">删除</el-button>
            </template>
          </el-table-column>
          <template #empty><el-empty description="还没有中继地址" :image-size="70" /></template>
        </el-table>

        <div class="list-cfg">
          <span>允许迁移</span>
          <el-switch v-model="allowLeave" />
          <span class="muted">关闭时新列表不含设备当前 relay 会被拒绝；开启会断开并迁移，谨慎使用</span>
        </div>

        <div class="foot">
          <span class="muted">已选 {{ selected.length }} 个</span>
          <el-button :disabled="!selected.length" @click="addSelected">加入下发列表</el-button>
          <span class="spacer" />
          <el-pagination v-model:current-page="dpage" :page-size="dper" :total="dtotal"
                         layout="prev, pager, next, total" background @current-change="loadDict" />
        </div>
        <div class="hint muted">
          顺序即故障切换顺序：当前 relay 断开后设备依次尝试后面的地址；设备上线时会自动对齐这份列表。
          保存只更新期望状态，立即下发请在「设备」页创建同步任务。
        </div>
      </el-tab-pane>

      <el-tab-pane label="设备" name="devices">
        <el-form :model="task" label-width="96" class="cfg">
          <el-form-item label="任务名称">
            <el-input v-model="task.name" maxlength="64" placeholder="中继列表同步" class="w240" />
          </el-form-item>
          <el-form-item label="开始时间">
            <el-date-picker v-model="task.start" type="datetime" value-format="x" />
          </el-form-item>
          <el-form-item label="下发方式">
            <el-radio-group v-model="task.mode">
              <el-radio-button value="once">一次性</el-radio-button>
              <el-radio-button value="wave">分批灰度</el-radio-button>
            </el-radio-group>
          </el-form-item>
          <template v-if="task.mode === 'wave'">
            <el-form-item label="每批台数">
              <el-input-number v-model="task.waveSize" :min="1" :max="50" controls-position="right" />
            </el-form-item>
            <el-form-item label="批间隔">
              <el-input-number v-model="task.intervalMin" :min="0" :max="1440" controls-position="right" />
              <span class="muted unit">分钟</span>
            </el-form-item>
            <el-form-item label="失败阈值">
              <el-input-number v-model="task.failPct" :min="0" :max="100" controls-position="right" />
              <span class="muted unit">%（超过则暂停）</span>
            </el-form-item>
          </template>
          <el-form-item label="截止时间">
            <el-date-picker v-model="task.deadline" type="datetime" value-format="x" />
          </el-form-item>
          <el-form-item label="最大重试">
            <el-input-number v-model="task.maxRetries" :min="0" :max="5" controls-position="right" />
          </el-form-item>
        </el-form>

        <div class="bar">
          <el-input v-model="devQ" placeholder="搜索设备" clearable class="w200">
            <template #prefix><el-icon><Search /></el-icon></template>
          </el-input>
          <el-checkbox v-model="onlyOnline">只看在线</el-checkbox>
          <span class="spacer" />
          <span class="muted">已选 {{ devSelected.length }} 台（在线 {{ devSelectedOnline }}）</span>
        </div>

        <el-table ref="devTable" :data="deviceRows" v-loading="devLoading" row-key="devid" @selection-change="onDevSelect">
          <el-table-column type="selection" width="44" reserve-selection />
          <el-table-column prop="devid" label="设备" :min-width="isPhone ? 110 : 150" />
          <el-table-column label="状态" :width="isPhone ? 70 : 90">
            <template #default="{ row }">
              <el-tag :type="row.online ? 'success' : 'info'" size="small">
                {{ row.online ? '在线' : '离线' }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column v-if="!isPhone" prop="version" label="版本" width="110">
            <template #default="{ row }">{{ row.version || '—' }}</template>
          </el-table-column>
          <el-table-column label="列表状态" :width="isPhone ? 110 : 130">
            <template #default="{ row }">
              <el-tag :type="listState(row).type" size="small">{{ listState(row).text }}</el-tag>
            </template>
          </el-table-column>
          <template #empty><el-empty description="没有匹配的设备" :image-size="70" /></template>
        </el-table>

        <div class="foot">
          <span class="muted">{{ preview }}</span>
          <span class="spacer" />
          <el-button type="primary" :disabled="!devSelected.length" :loading="creating" @click="createTask">
            创建同步任务（{{ devSelected.length }}）
          </el-button>
        </div>
        <div class="hint muted">
          离线设备会标记「待上线」，上线后自动对齐；失败率超过阈值任务会暂停，修好后点「继续」。
        </div>
      </el-tab-pane>
    </el-tabs>
  </div>

  <div class="panel tasks">
    <div class="panel-head">
      <h2>同步任务</h2>
      <span class="spacer" />
      <el-select v-model="taskStatus" class="w140" placeholder="全部" @change="loadTasks">
        <el-option label="全部" value="all" />
        <el-option label="进行中" value="active" />
        <el-option label="已暂停" value="paused" />
        <el-option label="已完成" value="done" />
        <el-option label="失败" value="failed_group" />
        <el-option label="已取消" value="cancelled" />
      </el-select>
      <el-button @click="loadTasks">刷新</el-button>
    </div>
    <el-table :data="tasks" v-loading="taskLoading" size="small">
      <el-table-column prop="name" label="任务" min-width="180">
        <template #default="{ row }">
          {{ row.name }}
          <div class="muted mono">{{ row.batch }}</div>
        </template>
      </el-table-column>
      <el-table-column prop="rev" label="列表版本" width="120" />
      <el-table-column label="批次" width="100">
        <template #default="{ row }">{{ row.mode === 'wave' ? row.current_wave + '/' + row.total_waves : '—' }}</template>
      </el-table-column>
      <el-table-column label="结果" min-width="150">
        <template #default="{ row }">
          <span>成功 {{ row.ok }}</span>
          <span class="muted"> · 失败 {{ row.failed + row.timeout }} · 待上线 {{ row.pending }}</span>
          <span v-if="row.active" class="muted"> · 进行 {{ row.active }}</span>
        </template>
      </el-table-column>
      <el-table-column prop="status" label="状态" width="110">
        <template #default="{ row }">
          <el-tag :type="taskState(row.status).type" size="small">{{ taskState(row.status).text }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="start_at_ms" label="开始" width="160">
        <template #default="{ row }">{{ time(row.start_at_ms) }}</template>
      </el-table-column>
      <el-table-column label="操作" width="200">
        <template #default="{ row }">
          <el-button link type="primary" @click="openTask(row)">详情</el-button>
          <el-button v-if="row.status === 'running' || row.status === 'scheduled'" link
                     @click="act(row, 'pause')">暂停</el-button>
          <el-button v-if="row.status === 'paused'" link @click="act(row, 'resume')">继续</el-button>
          <el-button v-if="row.status === 'running' || row.status === 'paused' || row.status === 'scheduled'"
                     link type="danger" @click="act(row, 'cancel')">取消</el-button>
          <el-button v-if="row.failed + row.timeout > 0" link @click="act(row, 'retry')">重试</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="还没有同步任务" :image-size="70" /></template>
    </el-table>
    <div class="foot">
      <el-pagination v-model:current-page="tpage" :page-size="20" :total="ttotal"
                     layout="prev, pager, next, total" background @current-change="loadTasks" />
    </div>
  </div>

  <el-drawer v-model="drawer" :title="detail ? detail.name : '任务详情'" size="680px">
    <template v-if="detail">
      <el-descriptions :column="1" border size="small">
        <el-descriptions-item label="任务号">{{ detail.batch }}</el-descriptions-item>
        <el-descriptions-item label="列表版本">{{ detail.rev }}</el-descriptions-item>
        <el-descriptions-item label="列表">{{ detail.list_json ? serversOf(detail.list_json) : '—' }}</el-descriptions-item>
        <el-descriptions-item label="策略">
          {{ detail.mode === 'wave'
            ? '分批：每批 ' + detail.wave_size + ' 台，间隔 ' + Math.round(detail.wave_interval_s / 60) +
              ' 分钟，失败阈值 ' + detail.fail_threshold_pct + '%'
            : '一次性下发' }}
        </el-descriptions-item>
        <el-descriptions-item label="状态">
          <el-tag :type="taskState(detail.status).type" size="small">{{ taskState(detail.status).text }}</el-tag>
          <span class="muted" v-if="detail.status === 'paused'"> 失败率超过阈值，修复后点「继续」</span>
        </el-descriptions-item>
      </el-descriptions>

      <el-table :data="detail.items" size="small" class="detail-table">
        <el-table-column prop="wave" label="批次" width="70" />
        <el-table-column prop="devid" label="设备" min-width="130" />
        <el-table-column label="状态" width="100">
          <template #default="{ row }">
            <el-tag :type="itemState(row.status).type" size="small">{{ itemState(row.status).text }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="ACK" width="110">
          <template #default="{ row }">
            <span v-if="row.ack_rev">rev {{ row.ack_rev }}</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <el-table-column label="结果" min-width="160">
          <template #default="{ row }">
            <span v-if="row.ack_error" class="err">{{ row.ack_error }}</span>
            <span v-else-if="row.error">{{ row.error }}</span>
            <span v-else-if="row.status === 'success' && row.persisted" class="muted">已写入设备配置</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
      </el-table>

      <div class="foot">
        <el-button v-if="detail.failed + detail.timeout > 0" @click="act(detail, 'retry')">重试失败项</el-button>
        <el-button v-if="detail.status === 'paused'" type="primary" @click="act(detail, 'resume')">继续</el-button>
        <span class="spacer" />
        <el-button @click="drawer = false">关闭</el-button>
      </div>
    </template>
  </el-drawer>

  <el-dialog v-model="dlg" :title="editId ? '编辑中继地址' : '新增中继地址'" :width="isPhone ? '92%' : '460px'">
    <el-form ref="formRef" :model="form" :rules="rules" label-width="80">
      <el-form-item label="地址" prop="host">
        <el-input v-model="form.host" placeholder="IP 或域名，IPv6 直接填裸地址" />
      </el-form-item>
      <el-form-item label="端口" prop="port">
        <el-input-number v-model="form.port" :min="1" :max="65535" controls-position="right" />
      </el-form-item>
      <el-form-item label="名称">
        <el-input v-model="form.name" maxlength="64" placeholder="如 主 relay / 备 relay" />
      </el-form-item>
      <el-form-item label="备注">
        <el-input v-model="form.description" maxlength="256" type="textarea" :rows="2" />
      </el-form-item>
      <el-form-item label="启用">
        <el-switch v-model="form.enabled" :disabled="!!editPos" />
        <span v-if="editPos" class="muted unit">在下发列表中，需先移出</span>
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="dlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="submitDict">保存</el-button>
    </template>
  </el-dialog>

  <el-dialog v-model="importDlg" title="批量导入中继地址" :width="isPhone ? '92%' : '520px'">
    <el-alert type="info" :closable="false" show-icon
              title="每行一条：地址 [名称] [备注]；地址支持 host、host:port、[v6]:port，重复的自动跳过" />
    <el-input v-model="importText" type="textarea" :rows="8" class="import"
              placeholder="10.0.0.2:18443 主 relay&#10;10.0.0.3:18443 备 relay" />
    <template #footer>
      <el-button @click="importDlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="submitImport">导入</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { time } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()

const tab = ref('dict')
const loading = ref(false)
const saving = ref(false)
const creating = ref(false)

// ---------------- 地址字典 + 有序下发列表
const dict = ref([])
const q = ref('')
const stateFilter = ref('all')
const dpage = ref(1)
const dper = 50
const dtotal = ref(0)
const selected = ref([])
const order = ref([])
const savedOrder = ref([])
const allowLeave = ref(false)
const savedLeave = ref(false)
const rev = ref(0)
const configured = ref(false)
const acks = ref({})
const onlineAgents = ref([])

const dirty = computed(() => {
  if (allowLeave.value !== savedLeave.value) return true
  if (order.value.length !== savedOrder.value.length) return true
  return order.value.some((id, i) => id !== savedOrder.value[i])
})
const posOf = (id) => order.value.indexOf(id) + 1
const dictRows = computed(() => dict.value.filter((r) => {
  if (stateFilter.value === 'in') return posOf(r.id) > 0
  if (stateFilter.value === 'out') return posOf(r.id) === 0
  if (stateFilter.value === 'off') return !r.enabled
  return true
}))

async function loadDict() {
  loading.value = true
  try {
    const j = await Api.get('/api/v1/relay-dict?page=' + dpage.value + '&page_size=' + dper +
      '&q=' + encodeURIComponent(q.value.trim()))
    dict.value = (j && j.items) || []
    dtotal.value = (j && j.total) || 0
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    loading.value = false
  }
}

async function loadList() {
  try {
    const j = await Api.get('/api/v1/relay-list')
    const ids = ((j && j.items) || []).map((x) => x.endpoint_id)
    order.value = ids
    savedOrder.value = ids.slice()
    allowLeave.value = !!(j && j.allow_leave)
    savedLeave.value = allowLeave.value
    rev.value = (j && j.rev) || 0
    configured.value = !!(j && j.configured)
    onlineAgents.value = (j && j.agents) || []
    const m = {}
    for (const a of onlineAgents.value) if (a.devid) m[a.devid] = a
    acks.value = m
  } catch (e) {
    ElMessage.error(e.message)
  }
}

function reloadAll() {
  loadDict()
  loadList()
  loadAgents()
  loadTasks()
}

function onDictSelect(rows) {
  selected.value = rows
}

function addSelected() {
  const next = order.value.slice()
  for (const r of selected.value) {
    if (next.includes(r.id)) continue
    if (next.length >= 8) {
      ElMessage.warning('下发列表最多 8 个地址')
      break
    }
    if (!r.enabled) {
      ElMessage.warning('已停用的地址不能加入下发列表：' + r.host)
      continue
    }
    next.push(r.id)
  }
  order.value = next
}

function removeFromList(id) {
  order.value = order.value.filter((x) => x !== id)
}

function move(id, delta) {
  const i = order.value.indexOf(id)
  const j = i + delta
  if (i < 0 || j < 0 || j >= order.value.length) return
  const next = order.value.slice()
  next[i] = next[j]
  next[j] = id
  order.value = next
}

async function saveList(silent) {
  try {
    await Api.put('/api/v1/relay-list', {
      endpoint_ids: order.value,
      allow_leave: allowLeave.value,
    })
    await loadList()
    if (!silent) ElMessage.success('已保存，设备上线时自动对齐')
    return true
  } catch (e) {
    ElMessage.error(e.message)
    return false
  }
}

// ---------------- 字典编辑
const dlg = ref(false)
const importDlg = ref(false)
const importText = ref('')
const editId = ref(0)
const formRef = ref(null)
const form = reactive({ host: '', port: 18443, name: '', description: '', enabled: true })
const editPos = computed(() => (editId.value ? posOf(editId.value) : 0))
const rules = {
  host: [{ required: true, message: '请填写地址', trigger: 'blur' }],
  port: [{ required: true, message: '请填写端口', trigger: 'blur' }],
}

function openAdd() {
  editId.value = 0
  Object.assign(form, { host: '', port: 18443, name: '', description: '', enabled: true })
  dlg.value = true
}

function openEdit(row) {
  editId.value = row.id
  Object.assign(form, {
    host: row.host,
    port: row.port,
    name: row.name || '',
    description: row.description || '',
    enabled: row.enabled,
  })
  dlg.value = true
}

async function submitDict() {
  if (!formRef.value) return
  const ok = await formRef.value.validate().catch(() => false)
  if (!ok) return
  saving.value = true
  try {
    const body = {
      host: form.host.trim(),
      port: form.port,
      name: form.name.trim(),
      description: form.description.trim(),
      enabled: form.enabled,
    }
    if (editId.value) await Api.put('/api/v1/relay-dict/' + editId.value, body)
    else await Api.post('/api/v1/relay-dict', body)
    dlg.value = false
    await loadDict()
    ElMessage.success('已保存')
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    saving.value = false
  }
}

async function removeDict(row) {
  try {
    await ElMessageBox.confirm('删除地址 ' + row.host + ':' + row.port + '？', '删除', { type: 'warning' })
  } catch (e) {
    return
  }
  try {
    await Api.del('/api/v1/relay-dict/' + row.id)
    if (posOf(row.id)) removeFromList(row.id)
    await loadDict()
    ElMessage.success('已删除')
  } catch (e) {
    ElMessage.error(e.message)
  }
}

function openImport() {
  importText.value = ''
  importDlg.value = true
}

async function submitImport() {
  if (!importText.value.trim()) {
    ElMessage.warning('请输入至少一行地址')
    return
  }
  saving.value = true
  try {
    const r = await Api.post('/api/v1/relay-dict/batch', { text: importText.value })
    importDlg.value = false
    await loadDict()
    ElMessage.success('新增 ' + r.added + ' 条，重复 ' + r.duplicated + ' 条，无效 ' + r.invalid + ' 条')
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    saving.value = false
  }
}

// ---------------- 设备与任务
const agents = ref([])
const devTable = ref(null)
const devLoading = ref(false)
const devQ = ref('')
const onlyOnline = ref(false)
const devSelected = ref([])
const taskStatus = ref('all')
const tasks = ref([])
const ttotal = ref(0)
const tpage = ref(1)
const taskLoading = ref(false)
const drawer = ref(false)
const detail = ref(null)

const task = reactive({
  name: '',
  start: Date.now(),
  mode: 'once',
  waveSize: 1,
  intervalMin: 10,
  failPct: 20,
  deadline: Date.now() + 24 * 3600 * 1000,
  maxRetries: 1,
})

const deviceRows = computed(() => agents.value.filter((d) => {
  if (onlyOnline.value && !d.online) return false
  const kw = devQ.value.trim().toLowerCase()
  if (kw && String(d.devid).toLowerCase().indexOf(kw) < 0) return false
  return true
}))
const devSelectedOnline = computed(() => devSelected.value.filter((d) => d.online).length)
const preview = computed(() => {
  const n = devSelected.value.length
  if (!n) return '选择设备后创建任务'
  if (task.mode === 'once') return '一次性下发 ' + n + ' 台'
  const size = Math.max(1, Number(task.waveSize) || 1)
  return n + ' 台 → ' + Math.ceil(n / size) + ' 批，每批 ' + size + ' 台，间隔 ' +
    (Number(task.intervalMin) || 0) + ' 分钟'
})

async function loadAgents() {
  devLoading.value = true
  try {
    const j = await Api.get('/api/v1/agents')
    agents.value = (Array.isArray(j) ? j : (j && j.items) || []).slice()
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    devLoading.value = false
  }
}

function versionAtLeast(v) {
  const m = String(v || '').match(/^(\d+)\.(\d+)\.(\d+)/)
  if (!m) return false
  const a = [+m[1], +m[2], +m[3]]
  const b = [1, 4, 98]
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) return a[i] > b[i]
  }
  return true
}

function listState(d) {
  if (!versionAtLeast(d.version)) return { text: '不支持下发', type: 'info' }
  if (!d.online) return { text: '离线', type: 'info' }
  const a = acks.value[d.devid]
  if (!a) return { text: '未响应', type: 'info' }
  // 已连接但还没回 ACK：conn 还在，不能当成“被拒绝”
  if (a.pending) return { text: '未响应', type: 'info' }
  if (a.ok && Number(a.rev) === Number(rev.value)) return { text: '已同步', type: 'success' }
  if (!a.ok) return { text: '被拒绝', type: 'danger' }
  return { text: '旧 rev', type: 'warning' }
}

function onDevSelect(rows) {
  devSelected.value = rows
}

async function createTask() {
  if (!order.value.length) {
    ElMessage.warning('下发列表为空，请先在中继地址页加入地址')
    return
  }
  if (dirty.value) {
    try {
      await ElMessageBox.confirm('下发列表有未保存的修改，先保存再创建任务？', '保存列表', { type: 'warning' })
    } catch (e) {
      return
    }
    if (!(await saveList(true))) return
  }
  creating.value = true
  try {
    const start = Number(task.start) || Date.now()
    const deadline = Number(task.deadline) || (start + 24 * 3600 * 1000)
    const r = await Api.post('/api/v1/relay-sync/tasks', {
      name: task.name || '中继列表同步',
      devids: devSelected.value.map((d) => d.devid),
      mode: task.mode,
      wave_size: task.mode === 'wave' ? task.waveSize : devSelected.value.length,
      wave_interval_s: (Number(task.intervalMin) || 0) * 60,
      fail_threshold_pct: Number(task.failPct) || 0,
      start_at_ms: start,
      deadline_at_ms: deadline,
      max_retries: Number(task.maxRetries) || 0,
    })
    ElMessage.success('已创建任务 ' + r.batch + '（' + r.total + ' 台）')
    task.name = ''
    devSelected.value = []
    if (devTable.value) devTable.value.clearSelection()
    await loadTasks()
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    creating.value = false
  }
}

async function loadTasks() {
  taskLoading.value = true
  try {
    const st = taskStatus.value && taskStatus.value !== 'all' ? '&status=' + taskStatus.value : ''
    const j = await Api.get('/api/v1/relay-sync/tasks?page=' + tpage.value + '&page_size=20' + st)
    tasks.value = (j && j.items) || []
    ttotal.value = (j && j.total) || 0
  } catch (e) {
    ElMessage.error(e.message)
  } finally {
    taskLoading.value = false
  }
}

async function openTask(row) {
  try {
    detail.value = await Api.get('/api/v1/relay-sync/tasks/' + row.batch)
    drawer.value = true
  } catch (e) {
    ElMessage.error(e.message)
  }
}

async function act(row, action) {
  const label = { pause: '暂停', resume: '继续', cancel: '取消', retry: '重试' }[action] || action
  try {
    await ElMessageBox.confirm(label + '任务 ' + (row.name || row.batch) + '？', label, { type: 'warning' })
  } catch (e) {
    return
  }
  try {
    await Api.post('/api/v1/relay-sync/tasks/' + row.batch + '/' + action)
    ElMessage.success('已' + label)
    await loadTasks()
    if (drawer.value && detail.value && detail.value.batch === row.batch) openTask(row)
  } catch (e) {
    ElMessage.error(e.message)
  }
}

function taskState(s) {
  const m = {
    scheduled: { text: '待开始', type: 'info' },
    running: { text: '进行中', type: 'warning' },
    paused: { text: '已暂停', type: 'danger' },
    success: { text: '成功', type: 'success' },
    partial: { text: '部分完成', type: 'warning' },
    failed: { text: '失败', type: 'danger' },
    cancelled: { text: '已取消', type: 'info' },
  }
  return m[s] || { text: s || '—', type: 'info' }
}

function itemState(s) {
  const m = {
    scheduled: { text: '待释放', type: 'info' },
    waiting: { text: '等下一批', type: 'info' },
    queued: { text: '排队', type: 'info' },
    running: { text: '已发', type: 'warning' },
    success: { text: '成功', type: 'success' },
    failed: { text: '失败', type: 'danger' },
    timeout: { text: '超时', type: 'danger' },
    pending: { text: '待上线', type: 'warning' },
    cancelled: { text: '已取消', type: 'info' },
  }
  return m[s] || { text: s || '—', type: 'info' }
}

function serversOf(js) {
  try {
    const j = JSON.parse(js)
    return j.servers || '—'
  } catch (e) {
    return '—'
  }
}

function onTab() {
  if (tab.value === 'devices') {
    loadAgents()
    loadList()
  }
}

let timer = 0
onMounted(async () => {
  await Promise.all([loadDict(), loadList(), loadAgents(), loadTasks()])
  timer = setInterval(() => {
    loadTasks(true)
    if (tab.value === 'devices') loadList()
  }, 8000)
})
onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.bar { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-bottom: 10px; }
.bar .spacer { flex: 1; }
.panel-head h2 { flex: none; white-space: nowrap; }
.list-cfg { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-top: 10px; }
.foot { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-top: 10px; }
.foot .spacer { flex: 1; }
.cfg { display: flex; flex-wrap: wrap; gap: 0 18px; }
.cfg .unit { margin-left: 6px; }
.w130 { width: 130px; }
.w200 { width: 200px; }
.w220 { width: 220px; }
.w240 { width: 240px; }
.hint { margin-top: 8px; line-height: 1.7; }
.mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
.pos { margin-right: 4px; }
.import { margin-top: 12px; }
.detail-table { margin-top: 12px; }
.err { color: var(--el-color-danger); }
.tasks { margin-top: 14px; }
@media (max-width: 720px) {
  .sum { display: none; }
}
</style>
