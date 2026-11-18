<template>
  <div class="panel">
    <div class="panel-head">
      <h2>Agent 升级（OTA）</h2>
      <span class="spacer" />
      <span class="muted">已选 {{ selected.size }} / 在线 {{ onlineCount }} / 共 {{ devices.length }}</span>
    </div>

    <div class="cfg">
      <label class="cfg-item">升级包（文件库 agent/）
        <div class="pick">
          <el-select v-model="fileName" filterable placeholder="选择 rtunnel-agent 二进制" class="w260"
                     @change="onFilePick">
            <el-option v-for="f in files" :key="f.name" :label="f.name" :value="f.name">
              <span>{{ f.name }}</span>
              <el-tag v-if="variantOf(f.name)" size="small" class="vt">{{ variantOf(f.name) }}</el-tag>
              <span class="muted" style="float:right">{{ bytes(f.size, true) }}</span>
            </el-option>
            <template #empty><span class="muted">agent 目录暂无升级包</span></template>
          </el-select>
          <el-button link type="primary" :loading="fileLoading" @click="loadFiles">刷新</el-button>
        </div>
      </label>
      <label class="cfg-item">目标版本
        <el-input v-model="version" placeholder="如 1.3.307" class="w160" />
      </label>
      <label class="cfg-item">截止时间
        <el-date-picker v-model="deadline" type="datetime" value-format="x" />
      </label>
      <label class="cfg-item">最大重试
        <el-input-number v-model="maxRetries" :min="0" :max="5" controls-position="right" />
      </label>
      <label class="cfg-item">强制重装
        <el-switch v-model="force" />
      </label>
      <el-button link type="primary" @click="$router.push('/files')">上传新版本 →</el-button>
    </div>
    <div class="hint muted">
      升级包放在文件库 <code>agent/</code> 目录，用 gzip 压缩后上传，命名
      <code>rtunnel-agent-lite-&lt;版本&gt;.gz</code> / <code>rtunnel-agent-web-&lt;版本&gt;.gz</code>；
      选择后会自动填入版本号，设备侧自动解压安装。
    </div>

    <el-alert type="warning" :closable="false" show-icon
              title="升级会让设备上的 agent 进程重新启动，正在进行的隧道会话会中断；仅支持开启 --allow-ota 的常驻 agent。"
              style="margin-bottom:12px" />

    <el-table :data="pageRows" v-loading="loading" row-key="devid" @selection-change="onSelect">
      <el-table-column type="selection" width="46" reserve-selection :selectable="(r) => r.caps_ota !== 0" />
      <el-table-column prop="devid" label="设备" min-width="150" />
      <el-table-column label="当前版本" width="120">
        <template #default="{ row }">{{ row.version || '—' }}</template>
      </el-table-column>
      <el-table-column label="OTA" width="90">
        <template #default="{ row }">
          <el-tag v-if="row.caps_ota === 1" type="success" size="small">支持</el-tag>
          <el-tag v-else-if="row.caps_ota === 0" type="info" size="small">未开启</el-tag>
          <el-tag v-else type="warning" size="small">未知</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="状态" width="90">
        <template #default="{ row }">
          <el-tag :type="row.online ? 'success' : 'info'" size="small">{{ row.online ? '在线' : '离线' }}</el-tag>
        </template>
      </el-table-column>
      <template #empty><el-empty description="没有匹配的设备" :image-size="70" /></template>
    </el-table>

    <div class="foot">
      <el-input v-model="q" placeholder="搜索设备" clearable class="w200">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-pagination v-model:current-page="page" :page-size="per" :total="filtered.length"
                     layout="prev, pager, next, total" background />
      <span class="spacer" />
      <el-button type="primary" :loading="saving" :disabled="!fileName || !version || !selected.size"
                 @click="submit">创建升级任务（{{ selected.size }}）</el-button>
    </div>
  </div>

  <div class="panel tasks">
    <div class="panel-head">
      <h2>升级任务</h2>
      <span class="spacer" />
      <el-select v-model="taskStatus" class="w140" placeholder="全部" @change="loadTasks">
        <el-option label="全部" value="" />
        <el-option label="进行中" value="active" />
        <el-option label="成功" value="done" />
        <el-option label="失败" value="failed_group" />
        <el-option label="冲突" value="conflict" />
      </el-select>
      <el-button @click="loadTasks">刷新</el-button>
    </div>
    <el-table :data="tasks" v-loading="taskLoading" size="small">
      <el-table-column prop="devid" label="设备" min-width="140" />
      <el-table-column prop="target_version" label="目标版本" width="110" />
      <el-table-column label="状态" width="100">
        <template #default="{ row }">
          <el-tag :type="statusType(row.status)" size="small">{{ statusText(row.status) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="进度" width="150">
        <template #default="{ row }">
          <el-progress :percentage="taskPercent(row)" :stroke-width="6"
                       :status="row.status === 'success' ? 'success' : (row.status === 'failed' || row.status === 'timeout' ? 'exception' : '')" />
        </template>
      </el-table-column>
      <el-table-column label="错误" min-width="200">
        <template #default="{ row }"><span class="muted">{{ row.error || '—' }}</span></template>
      </el-table-column>
      <el-table-column label="时间" width="160">
        <template #default="{ row }">{{ ts(row.finished_ms || row.created_ms) }}</template>
      </el-table-column>
      <el-table-column label="操作" width="130" fixed="right">
        <template #default="{ row }">
          <el-button v-if="canCancel(row.status)" link type="danger" size="small" @click="cancel(row)">取消</el-button>
          <el-button v-if="canRetry(row.status, row.kind)" link type="primary" size="small" @click="retry(row)">重试</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无升级任务" :image-size="70" /></template>
    </el-table>
    <div class="foot">
      <el-pagination v-model:current-page="taskPage" :page-size="taskPer" :total="taskTotal"
                     layout="prev, pager, next, total" background @current-change="loadTasks" />
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { bytes } from '@/shared/utils.js'
import {
  taskCanCancel as canCancel, taskCanRetry as canRetry, taskPercent,
  taskStatusText as statusText, taskStatusType as statusType,
} from '@/shared/tasks.js'

const files = ref([])
const fileLoading = ref(false)
let autoVersion = ''
const devices = ref([])
const loading = ref(false)
const saving = ref(false)
const fileName = ref('')
const version = ref('')
const deadline = ref(Date.now() + 3600 * 1000)
const maxRetries = ref(2)
const force = ref(false)
const q = ref('')
const page = ref(1)
const per = 10
const selected = ref(new Set())

const tasks = ref([])
const taskLoading = ref(false)
const taskStatus = ref('')
const taskPage = ref(1)
const taskPer = 20
const taskTotal = ref(0)
let timer = null

const onlineCount = computed(() => devices.value.filter((d) => d.online).length)
const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return devices.value
  return devices.value.filter((d) => String(d.devid).toLowerCase().includes(k))
})
const pageRows = computed(() => filtered.value.slice((page.value - 1) * per, page.value * per))

function ts(ms) {
  if (!ms) return '—'
  return new Date(Number(ms)).toLocaleString()
}

function onSelect(rows) {
  const current = new Set(pageRows.value.map((r) => r.devid))
  current.forEach((id) => selected.value.delete(id))
  rows.forEach((r) => selected.value.add(r.devid))
}

async function loadFiles() {
  fileLoading.value = true
  try {
    const fl = await Api.get('/api/v1/files?dir=agent').catch(() => [])
    const list = (fl || []).filter((f) => !f.type || f.type === 'file')
    files.value = list
    if (!list.some((f) => f.name === fileName.value)) {
      fileName.value = list.length ? list[0].name : ''
      onFilePick(fileName.value)
    }
  } finally {
    fileLoading.value = false
  }
}

// 从文件名末尾取版本号，例如 rtunnel-agent-lite-1.4.3.gz
function versionOf(name) {
  const base = String(name || '').replace(/\.gz$/i, '')
  const m = base.match(/(\d+\.\d+\.\d+(?:[-+][\w.]+)?)$/)
  return m ? m[1] : ''
}

function variantOf(name) {
  const n = String(name || '')
  if (n.includes('-web-') || n.endsWith('-web')) return 'web'
  if (n.includes('-lite-') || n.endsWith('-lite')) return 'lite'
  return ''
}

function onFilePick(name) {
  const v = versionOf(name)
  if (v && (!version.value || version.value === autoVersion)) version.value = v
  autoVersion = v
}

async function load() {
  loading.value = true
  try {
    const [ag] = await Promise.all([
      Api.get('/api/v1/agents').catch(() => []),
    ])
    await loadFiles()
    devices.value = (ag || []).map((a) => ({
      devid: a.devid,
      online: true,
      version: a.version || '',
      caps_ota: typeof a.caps_ota === 'number' ? a.caps_ota : -1,
    })).sort((a, b) => (a.devid < b.devid ? -1 : 1))
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function loadTasks() {
  taskLoading.value = true
  try {
    const r = await Api.get('/api/v1/distributions?kind=ota&page=' + taskPage.value +
      '&per_page=' + taskPer +
      (taskStatus.value ? '&status=' + encodeURIComponent(taskStatus.value) : ''))
    tasks.value = r.items || []
    taskTotal.value = r.total || 0
  } catch (e) {
    // 轮询失败不打扰
  } finally {
    taskLoading.value = false
  }
}

async function submit() {
  if (!fileName.value || !version.value || !selected.value.size) return
  const end = Number(deadline.value) || 0
  if (!end || end <= Date.now()) {
    ElMessage.warning('截止时间必须晚于当前时间')
    return
  }
  saving.value = true
  try {
    const r = await Api.post('/api/v1/distributions', {
      kind: 'ota',
      file: 'agent/' + fileName.value,
      version: version.value.trim(),
      devids: Array.from(selected.value),
      deadline_at_ms: end,
      max_retries: Number(maxRetries.value) || 0,
      force: force.value,
    })
    if (r.unsupported && r.unsupported.length) ElMessage.warning('部分设备未开启 OTA：' + r.unsupported)
    ElMessage.success('已创建 ' + r.tasks.length + ' 条升级任务')
    selected.value = new Set()
    taskPage.value = 1
    await loadTasks()
  } catch (e) {
    ElMessage.error('创建失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function cancel(row) {
  try {
    await Api.post('/api/v1/distributions/' + row.id + '/cancel', {})
    ElMessage.success('已请求取消')
    await loadTasks()
  } catch (e) {
    ElMessage.error('取消失败：' + e.message)
  }
}

async function retry(row) {
  try {
    await Api.post('/api/v1/distributions/' + row.id + '/retry', {})
    ElMessage.success('已重新入队')
    await loadTasks()
  } catch (e) {
    ElMessage.error('重试失败：' + e.message)
  }
}

onMounted(async () => {
  await load()
  await loadTasks()
  timer = setInterval(() => {
    if (document.visibilityState === 'visible') loadTasks()
  }, 3000)
})

onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.cfg { display: flex; flex-wrap: wrap; gap: 10px 16px; align-items: flex-end; margin-bottom: 12px; }
.pick { display: flex; align-items: center; gap: 4px; }
.pick .vt { margin-left: 6px; }
.hint { margin: -4px 0 12px; font-size: 12px; }
.hint code { padding: 1px 4px; border-radius: 4px; background: var(--el-fill-color-light); }
.cfg-item { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--el-text-color-secondary); }
.w160 { width: 150px; }
.w200 { width: 200px; }
.w260 { width: 260px; }
.foot { display: flex; align-items: center; gap: 10px; margin-top: 12px; flex-wrap: wrap; }
.foot .spacer { flex: 1; }
.tasks { margin-top: 16px; }
@media (max-width: 599px) {
  .w260, .w160, .w200 { width: 100%; }
}
</style>
