<template>
  <div class="panel">
    <div class="panel-head">
      <h2>操作作业</h2>
      <span class="muted">共 {{ items.length }} 条</span>
      <span class="spacer" />
      <el-select v-model="devid" clearable filterable placeholder="全部设备" class="sel" @change="load">
        <el-option v-for="d in devices" :key="d.devid" :label="d.devid" :value="d.devid" />
      </el-select>
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>

    <el-tabs v-model="tab" @tab-change="load">
      <el-tab-pane v-for="t in tabs" :key="t.key" :name="t.key" :label="t.label" />
    </el-tabs>

    <el-table :data="items" v-loading="loading" row-key="id">
      <el-table-column prop="id" label="ID" width="68" />
      <el-table-column label="设备" min-width="110">
        <template #default="{ row }">{{ row.target_id }}</template>
      </el-table-column>
      <el-table-column label="命令" min-width="220">
        <template #default="{ row }">
          <span class="cmd">{{ cmdline(row) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="风险" width="80">
        <template #default="{ row }">
          <el-tag :type="riskType(row.risk)" size="small">{{ riskText(row.risk) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="状态" width="110">
        <template #default="{ row }">
          <el-tag :type="statusType(row.status)" size="small" :title="row.error || ''">
            {{ statusText(row.status) }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="发起人" min-width="140" show-overflow-tooltip>
        <template #default="{ row }">
          {{ row.actor }}
          <el-tag v-if="row.actor_type === 'ai'" type="warning" size="small" class="ml4">AI</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="审批人" min-width="120" show-overflow-tooltip>
        <template #default="{ row }">{{ row.approver || '—' }}</template>
      </el-table-column>
      <el-table-column label="时间" width="150">
        <template #default="{ row }">{{ time(row.updated_ms) }}</template>
      </el-table-column>
      <el-table-column label="操作" width="190">
        <template #default="{ row }">
          <el-button v-if="canApprove(row)" link type="primary" @click="approve(row)">批准</el-button>
          <el-button v-if="canApprove(row)" link type="danger" @click="reject(row)">拒绝</el-button>
          <el-button v-if="canApply(row)" link type="primary" @click="apply(row)">执行</el-button>
          <el-button link @click="openDetail(row)">详情</el-button>
        </template>
      </el-table-column>
      <template #empty><span class="muted">没有符合条件的作业</span></template>
    </el-table>

    <el-dialog v-model="dlg" :title="'作业 #' + (cur.id || '')" width="640px">
      <el-descriptions :column="1" border size="small">
        <el-descriptions-item label="设备">{{ cur.target_id }}</el-descriptions-item>
        <el-descriptions-item label="状态">
          {{ statusText(cur.status) }} · 风险 {{ riskText(cur.risk) }}
        </el-descriptions-item>
        <el-descriptions-item label="发起人">{{ cur.actor }}</el-descriptions-item>
        <el-descriptions-item v-if="cur.approver" label="审批人">
          {{ cur.approver }}<span v-if="cur.approval_note">（{{ cur.approval_note }}）</span>
        </el-descriptions-item>
        <el-descriptions-item label="plan_hash">
          <code class="hash">{{ cur.plan_hash }}</code>
        </el-descriptions-item>
        <el-descriptions-item label="计划">
          <pre class="json">{{ pretty(cur.plan) }}</pre>
        </el-descriptions-item>
        <el-descriptions-item v-if="cur.result" label="结果">
          <pre class="json">{{ pretty(cur.result) }}</pre>
        </el-descriptions-item>
        <el-descriptions-item v-if="cur.error" label="错误">
          <span class="err">{{ cur.error }}</span>
        </el-descriptions-item>
      </el-descriptions>
      <template #footer>
        <el-button @click="dlg = false">关闭</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { time } from '@/shared/utils.js'

const tab = ref('pending_approval')
const devid = ref('')
const devices = ref([])
const items = ref([])
const loading = ref(false)
const dlg = ref(false)
const cur = ref({})

const tabs = [
  { key: 'pending_approval', label: '待审批' },
  { key: 'planned', label: '待执行' },
  { key: 'applied', label: '已执行' },
  { key: 'failed', label: '失败' },
  { key: '', label: '全部' },
]

function cmdline(row) {
  const argv = (row.plan && row.plan.argv) || []
  const s = argv.join(' ')
  return s.length > 80 ? s.slice(0, 80) + '…' : (s || '—')
}

function riskText(r) {
  return r >= 3 ? '破坏性' : (r === 2 ? '改状态' : '只读')
}

function riskType(r) {
  return r >= 3 ? 'danger' : (r === 2 ? 'warning' : 'info')
}

const STATUS_TEXT = {
  planned: '待执行', pending_approval: '待审批', approved: '已批准', rejected: '已拒绝',
  running: '执行中', applied: '已执行', verified: '已复核', failed: '失败',
}
const STATUS_TYPE = {
  planned: 'info', pending_approval: 'warning', approved: 'primary', rejected: 'danger',
  running: 'warning', applied: 'success', verified: 'success', failed: 'danger',
}

function statusText(s) { return STATUS_TEXT[s] || s }
function statusType(s) { return STATUS_TYPE[s] || 'info' }

function canApprove(row) {
  return row.status === 'pending_approval' && can('op.approve')
}

function canApply(row) {
  return (row.status === 'planned' || row.status === 'approved') && can('device.exec')
}

async function load() {
  loading.value = true
  try {
    let url = '/api/v1/operations?limit=200'
    if (tab.value) url += '&status=' + encodeURIComponent(tab.value)
    if (devid.value) url += '&devid=' + encodeURIComponent(devid.value)
    const r = await Api.get(url)
    items.value = r.items || []
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function loadDevices() {
  if (!can('device.view')) return
  const r = await Api.get('/api/v1/agents').catch(() => [])
  devices.value = Array.isArray(r) ? r : (r.items || [])
}

async function approve(row) {
  try {
    const r = await ElMessageBox.prompt('批准后即可执行，可填写备注', '批准作业 #' + row.id, {
      confirmButtonText: '批准', cancelButtonText: '取消', inputPlaceholder: '备注（可选）',
    })
    await Api.post('/api/v1/operations/' + row.id + '/approve',
      { decision: 'approve', note: (r.value || '').trim() })
    ElMessage.success('已批准')
    await load()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('批准失败：' + (e.message || e))
  }
}

async function reject(row) {
  try {
    const r = await ElMessageBox.prompt('请填写拒绝原因', '拒绝作业 #' + row.id, {
      confirmButtonText: '拒绝', cancelButtonText: '取消', inputPlaceholder: '原因',
      inputValidator: (v) => (v && v.trim() ? true : '必须填写原因'),
    })
    await Api.post('/api/v1/operations/' + row.id + '/approve',
      { decision: 'reject', note: r.value.trim() })
    ElMessage.success('已拒绝')
    await load()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('拒绝失败：' + (e.message || e))
  }
}

async function apply(row) {
  try {
    await ElMessageBox.confirm('将在 ' + row.target_id + ' 上执行：' + cmdline(row), '执行确认', {
      confirmButtonText: '执行', cancelButtonText: '取消', type: 'warning',
    })
  } catch (e) {
    return
  }
  try {
    await Api.post('/api/v1/operations/' + row.id + '/apply', { plan_hash: row.plan_hash })
    ElMessage.success('已提交执行')
    await load()
  } catch (e) {
    ElMessage.error('执行失败：' + e.message)
  }
}

function pretty(o) {
  try { return JSON.stringify(o, null, 2) } catch (e) { return String(o) }
}

function openDetail(row) {
  cur.value = row
  dlg.value = true
}

let timer = null
onMounted(() => {
  loadDevices()
  load()
  timer = setInterval(() => { if (!document.hidden) load() }, 3000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.sel { width: 170px; }
.cmd { font-family: var(--el-font-family-mono, monospace); font-size: 12.5px; }
.ml4 { margin-left: 4px; }
.hash { font-size: 11.5px; word-break: break-all; }
.json {
  margin: 0; max-height: 220px; overflow: auto; font-size: 12px;
  white-space: pre-wrap; word-break: break-all;
}
.err { color: var(--el-color-danger); }
@media (max-width: 599px) {
  .sel { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
