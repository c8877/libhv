<template>
  <div class="panel">
    <div class="panel-head">
      <h2>任务</h2>
      <span class="muted">共 {{ total }} 条</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索设备 / 文件" clearable class="search">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>

    <div v-if="batch" class="batch">
      <el-tag closable @close="clearBatch">本批：{{ batch }}</el-tag>
    </div>

    <el-tabs v-model="tab" @tab-change="onTab">
      <el-tab-pane v-for="t in tabs" :key="t.key" :name="t.key">
        <template #label>{{ t.label }} <span class="cnt">{{ t.count }}</span></template>
      </el-tab-pane>
    </el-tabs>

    <el-table :data="items" v-loading="loading">
      <el-table-column v-if="!isPhone" prop="id" label="ID" width="60" />
      <el-table-column prop="file_name" label="文件" min-width="130">
        <template #default="{ row }">
          {{ row.file_name }}
          <div v-if="isPhone" class="muted">{{ row.devid }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="devid" label="设备" min-width="100" />
      <el-table-column v-if="!isPhone" prop="target_path" label="目标路径" min-width="130">
        <template #default="{ row }">{{ row.target_path || '—' }}</template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="进度 / 大小" min-width="120">
        <template #default="{ row }">{{ progressText(row) }}</template>
      </el-table-column>
      <el-table-column label="状态" :min-width="isPhone ? 100 : 110">
        <template #default="{ row }">
          <el-tag :type="statusType(row.status)" size="small" :title="row.error || ''">
            {{ statusText(row.status) }}
          </el-tag>
          <div v-if="isPhone" class="muted">{{ progressText(row) }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="重试" width="70">
        <template #default="{ row }">{{ row.retry_count }} / {{ row.max_retries }}</template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="截止" min-width="140">
        <template #default="{ row }">{{ time(row.deadline_at_ms) }}</template>
      </el-table-column>
      <el-table-column :min-width="isPhone ? 92 : 210" label="操作">
        <template #default="{ row }">
          <el-dropdown v-if="isPhone" trigger="click" @command="(c) => onAction(c, row)">
            <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item command="detail">详情</el-dropdown-item>
                <el-dropdown-item v-if="row.status === 'failed'" command="retry">重试</el-dropdown-item>
                <el-dropdown-item v-if="row.status === 'scheduled' || row.status === 'queued'" command="cancel">取消</el-dropdown-item>
                <el-dropdown-item v-if="row.status === 'timeout' && row.extend_count === 0" command="extend">延长</el-dropdown-item>
                <el-dropdown-item v-if="row.status === 'conflict'" command="verify">校验</el-dropdown-item>
                <el-dropdown-item v-if="row.status === 'conflict'" command="force">强制重发</el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
          <template v-else>
            <el-button link type="primary" @click="$router.push('/tasks/' + row.id)">详情</el-button>
            <el-button v-if="row.status === 'failed'" size="small" @click="retry(row)">重试</el-button>
            <el-button v-if="row.status === 'scheduled' || row.status === 'queued'" size="small" @click="cancel(row)">取消</el-button>
            <el-button v-if="row.status === 'timeout' && row.extend_count === 0" size="small" @click="extend(row)">延长</el-button>
            <el-button v-if="row.status === 'conflict'" size="small" @click="check(row, 'verify')">校验</el-button>
            <el-button v-if="row.status === 'conflict'" size="small" @click="check(row, 'force')">强制重发</el-button>
          </template>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无任务" :image-size="70" /></template>
    </el-table>

    <div class="foot">
      <el-pagination v-model:current-page="page" :page-size="per" :total="total"
                     layout="prev, pager, next, total" background @current-change="load" />
    </div>
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { bytes, time } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const route = useRoute()
const router = useRouter()
const { isPhone } = useBreakpoint()

const tab = ref('all')
const q = ref('')
const page = ref(1)
const per = 20
const batch = ref(String(route.query.batch || ''))
const items = ref([])
const total = ref(0)
const loading = ref(false)
const counts = ref({})

const tabs = ref([
  { key: 'all', label: '全部', count: 0 },
  { key: 'active', label: '进行中', count: 0 },
  { key: 'done', label: '已完成', count: 0 },
  { key: 'failed_group', label: '失败', count: 0 },
  { key: 'conflict', label: '冲突', count: 0 },
])

const STATUS = {
  scheduled: ['等待开始', 'warning'], queued: ['排队中', 'warning'], running: ['传输中', 'warning'],
  success: ['成功', 'success'], failed: ['失败', 'danger'], timeout: ['超时', 'danger'],
  canceled: ['已取消', 'info'], conflict: ['冲突', 'warning'],
}
function statusText(s) { return (STATUS[s] || ['未知', 'warning'])[0] }
function statusType(s) { return (STATUS[s] || ['未知', 'warning'])[1] }

function progressText(t) {
  const size = Number(t.file_size) || 0
  const sent = Number(t.sent) || 0
  if (t.status !== 'running') return bytes(size)
  const pct = size ? Math.min(100, Math.floor((sent * 100) / size)) : 0
  return bytes(sent) + ' / ' + bytes(size) + ' ' + pct + '%'
}

async function load(silent) {
  if (!silent) loading.value = true
  let url = '/api/v1/distributions?page=' + page.value + '&per_page=' + per
  if (tab.value !== 'all') url += '&status=' + tab.value
  if (q.value.trim()) url += '&q=' + encodeURIComponent(q.value.trim())
  if (batch.value) url += '&batch=' + encodeURIComponent(batch.value)
  try {
    const [r, sum] = await Promise.all([
      Api.get(url),
      Api.get('/api/v1/distributions/summary').catch(() => ({})),
    ])
    items.value = r.items || []
    total.value = r.total || 0
    counts.value = sum || {}
    counts.value.all = sum.all
    tabs.value = [
      { key: 'all', label: '全部', count: sum.all || 0 },
      { key: 'active', label: '进行中', count: sum.active || 0 },
      { key: 'done', label: '已完成', count: sum.done || 0 },
      { key: 'failed_group', label: '失败', count: sum.failed || 0 },
      { key: 'conflict', label: '冲突', count: sum.conflict || 0 },
    ]
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

let debounce = null
function onTab() { page.value = 1; load() }
function clearBatch() {
  batch.value = ''
  page.value = 1
  load()
}

import { watch } from 'vue'
watch(q, () => {
  clearTimeout(debounce)
  debounce = setTimeout(() => { page.value = 1; load(true) }, 250)
})

async function retry(row) {
  try {
    await Api.post('/api/v1/distributions/' + row.id + '/retry', {})
    ElMessage.success('已重试')
    await load(true)
  } catch (e) { ElMessage.error('重试失败：' + e.message) }
}

async function cancel(row) {
  try {
    await Api.post('/api/v1/distributions/' + row.id + '/cancel', {})
    ElMessage.success('已取消')
    await load(true)
  } catch (e) { ElMessage.error('取消失败：' + e.message) }
}

async function extend(row) {
  let hours
  try {
    const r = await ElMessageBox.prompt('新的截止时间 = 现在 + N 小时（最多 24）', '延长截止时间', {
      inputValue: '24',
      inputPattern: /^([1-9]|1[0-9]|2[0-4])$/,
      inputErrorMessage: '请输入 1-24',
      confirmButtonText: '延长',
      cancelButtonText: '取消',
    })
    hours = Number(r.value)
  } catch (e) { return }
  try {
    await Api.post('/api/v1/distributions/' + row.id + '/extend',
      { deadline_at_ms: Date.now() + hours * 3600 * 1000 })
    ElMessage.success('已延长')
    await load(true)
  } catch (e) { ElMessage.error('延长失败：' + e.message) }
}

async function check(row, api) {
  try {
    const r = await Api.post('/api/v1/distributions/' + row.id + '/' + api, {})
    ElMessage.success(r.same ? '设备文件一致，已标记成功' : '设备文件不一致，已重新排队')
    await load(true)
  } catch (e) { ElMessage.error('操作失败：' + e.message) }
}

function onAction(cmd, row) {
  if (cmd === 'detail') router.push('/tasks/' + row.id)
  else if (cmd === 'retry') retry(row)
  else if (cmd === 'cancel') cancel(row)
  else if (cmd === 'extend') extend(row)
  else if (cmd === 'verify' || cmd === 'force') check(row, cmd)
}

let timer = null
onMounted(() => {
  load()
  timer = setInterval(() => { if (!document.hidden) load(true) }, 2000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.search { width: 220px; }
.batch { margin-bottom: 8px; }
.cnt {
  display: inline-block; min-width: 18px; padding: 0 6px; margin-left: 4px;
  border-radius: 9px; background: var(--el-fill-color);
  color: var(--el-text-color-secondary); font-size: 11px; line-height: 18px; text-align: center;
}
.foot { display: flex; justify-content: flex-end; margin-top: 12px; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
