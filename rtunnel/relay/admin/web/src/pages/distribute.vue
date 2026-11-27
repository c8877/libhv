<template>
  <div v-if="!fileName" class="panel">
    <div class="panel-head">
      <h2>分发文件</h2>
      <span class="muted">从文件仓库选一个文件，再挑要分发到的设备</span>
      <span class="spacer" />
      <el-input v-model="pickQ" placeholder="搜索文件名" clearable class="w220">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button :loading="pickLoading" @click="loadPick">刷新</el-button>
    </div>

    <div class="crumbs">
      <el-button link @click="openDir('')">文件仓库</el-button>
      <template v-for="c in crumbs" :key="c.path">
        <span class="sep">/</span>
        <el-button link @click="openDir(c.path)">{{ c.name }}</el-button>
      </template>
    </div>

    <el-table :data="pickShown" v-loading="pickLoading" size="small">
      <el-table-column label="名称" min-width="260">
        <template #default="{ row }">
          <el-button v-if="row.type === 'dir'" link type="primary" @click="openDir(row.path)">
            <el-icon class="ico"><FolderOpened /></el-icon>{{ row.name }}
          </el-button>
          <span v-else class="fname">
            <el-icon class="ico"><Document /></el-icon>{{ row.name }}
          </span>
        </template>
      </el-table-column>
      <el-table-column label="大小" width="110">
        <template #default="{ row }">
          <span class="muted">{{ row.type === 'dir' ? '—' : bytes(row.size) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="修改时间" width="170">
        <template #default="{ row }"><span class="muted">{{ when(row.mtime_ms) }}</span></template>
      </el-table-column>
      <el-table-column label="操作" width="120">
        <template #default="{ row }">
          <el-button v-if="row.type === 'file'" link type="primary" @click="pick(row)">分发…</el-button>
          <el-button v-else link @click="openDir(row.path)">进入</el-button>
        </template>
      </el-table-column>
      <template #empty>
        <el-empty :description="pickQ ? '这个目录没有匹配的文件' : '这个目录还没有文件'" :image-size="70">
          <el-button @click="$router.push('/files')">去文件仓库上传</el-button>
        </el-empty>
      </template>
    </el-table>
  </div>

  <div v-else>
    <div class="back">
      <el-button link @click="$router.replace('/distribute')">← 换一个文件</el-button>
      <el-button link @click="$router.push('/files')">文件列表</el-button>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>分发文件：{{ fileName }}</h2>
        <span class="spacer" />
        <span class="muted">已选 {{ selected.size }} / 在线 {{ onlineCount }} / 共 {{ devices.length }}</span>
      </div>

      <div class="cfg">
        <el-input v-model="q" placeholder="搜索设备" clearable class="w220">
          <template #prefix><el-icon><Search /></el-icon></template>
        </el-input>
        <el-input v-model="dir" placeholder="目标子目录（可选）" class="w220" />
        <label class="cfg-item">开始时间
          <el-date-picker v-model="startAt" type="datetime" value-format="x" />
        </label>
        <label class="cfg-item">结束时间
          <el-date-picker v-model="endAt" type="datetime" value-format="x" />
        </label>
        <label class="cfg-item">最大重试
          <el-input-number v-model="maxRetries" :min="0" :max="10" controls-position="right" />
        </label>
      </div>

      <el-table :data="pageRows" v-loading="loading" row-key="devid"
                :default-sort="{ prop: 'devid', order: 'ascending' }"
                @sort-change="onSortChange" @selection-change="onSelect">
        <el-table-column type="selection" width="46" reserve-selection :selectable="(r) => r.online" />
        <el-table-column prop="devid" label="设备" min-width="140" sortable="custom"
                       :sort-orders="TEXT_ORDERS" />
        <el-table-column prop="online" label="状态" width="90" sortable="custom"
                         :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <el-tag :type="row.online ? 'success' : 'info'" size="small">{{ row.online ? '在线' : '离线' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150"
                       sortable="custom" :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.remote_addr || '—' }}</template>
        </el-table-column>
        <template #empty><el-empty description="没有匹配的设备" :image-size="70" /></template>
      </el-table>

      <div class="foot">
        <el-pagination v-model:current-page="page" :page-size="per" :total="filtered.length"
                       layout="prev, pager, next, total" background />
        <span class="spacer" />
        <el-button @click="$router.push('/files')">取消</el-button>
        <el-button type="primary" :loading="saving" @click="submit">创建分发任务（{{ selected.size }}）</el-button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { NUM_ORDERS, TEXT_ORDERS, sortByOrder } from '@/shared/tableSort.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const route = useRoute()
const router = useRouter()
const { isPhone } = useBreakpoint()
const fileName = computed(() => String(route.query.file || ''))

// ---------------------------------------------------------------- 选文件
const pickDir = ref('')
const pickRows = ref([])
const pickLoading = ref(false)
const pickQ = ref('')

const crumbs = computed(() => {
  const seg = pickDir.value.split('/').filter(Boolean)
  const out = []
  let cur = ''
  for (const s of seg) {
    cur += '/' + s
    out.push({ name: s, path: cur })
  }
  return out
})

const pickShown = computed(() => {
  const k = pickQ.value.trim().toLowerCase()
  if (!k) return pickRows.value
  return pickRows.value.filter((r) => r.name.toLowerCase().includes(k))
})

async function loadPick() {
  pickLoading.value = true
  try {
    const arr = await Api.get('/api/v1/files?dir=' + encodeURIComponent(pickDir.value))
    pickRows.value = (arr || []).map((f) => ({ ...f, type: f.type === 'dir' ? 'dir' : 'file' }))
  } catch (e) {
    pickRows.value = []
    ElMessage.error('读文件仓库失败：' + e.message)
  } finally {
    pickLoading.value = false
  }
}

function openDir(dir) {
  pickDir.value = dir || ''
  pickQ.value = ''
  loadPick()
}

function pick(row) {
  router.push({ path: '/distribute', query: { file: row.path } })
}

function bytes(n) {
  if (!n && n !== 0) return '—'
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MiB'
  if (n >= 1024) return (n / 1024).toFixed(0) + ' KiB'
  return n + ' B'
}

function when(ms) {
  if (!ms) return '—'
  const d = new Date(ms)
  const p = (x) => String(x).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

const devices = ref([])
const loading = ref(false)
const saving = ref(false)
const q = ref('')
const dir = ref('')
const maxRetries = ref(3)
const now = Date.now()
const startAt = ref(now)
const endAt = ref(now + 24 * 3600 * 1000)
const page = ref(1)
const per = 10
const selected = ref(new Set())

const onlineCount = computed(() => devices.value.filter((d) => d.online).length)
const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return devices.value
  return devices.value.filter((d) => String(d.devid).toLowerCase().includes(k))
})
const sort = reactive({ prop: '', order: '' })
const sorted = computed(() => sortByOrder(filtered.value, sort.prop, sort.order, {
  online: (r) => (r.online ? 1 : 0)
}))
const pageRows = computed(() => sorted.value.slice((page.value - 1) * per, page.value * per))
function onSortChange({ prop, order }) {
  sort.prop = prop || ''
  sort.order = order || ''
  page.value = 1
}

watch(q, () => { page.value = 1 })

async function load() {
  loading.value = true
  try {
    const [agents, devs] = await Promise.all([
      Api.get('/api/v1/agents'),
      Api.get('/api/v1/devices').catch(() => []),
    ])
    const map = new Map()
    ;(agents || []).forEach((a) => map.set(a.devid, {
      devid: a.devid, online: true, remote_addr: a.remote_addr || '',
    }))
    ;(devs || []).forEach((d) => {
      if (!map.has(d.devid)) map.set(d.devid, { devid: d.devid, online: false, remote_addr: '' })
    })
    devices.value = Array.from(map.values()).sort((a, b) => (a.devid < b.devid ? -1 : a.devid > b.devid ? 1 : 0))
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onSelect(rows) {
  const current = new Set(pageRows.value.map((r) => r.devid))
  current.forEach((id) => selected.value.delete(id))
  rows.forEach((r) => selected.value.add(r.devid))
}

async function submit() {
  if (!selected.value.size) {
    ElMessage.warning('请选择设备')
    return
  }
  const start = Number(startAt.value) || 0
  const end = Number(endAt.value) || 0
  if (!start || !end || end <= start) {
    ElMessage.warning('时间范围不合法')
    return
  }
  if (end - start > 30 * 86400 * 1000) {
    ElMessage.warning('时间窗口不能超过 30 天')
    return
  }
  saving.value = true
  try {
    const r = await Api.post('/api/v1/distributions', {
      file: fileName.value,
      dir: dir.value.trim(),
      devids: Array.from(selected.value),
      start_at_ms: start,
      deadline_at_ms: end,
      max_retries: Number(maxRetries.value) || 0,
    })
    if (r.skipped_offline) ElMessage.warning('部分设备离线已跳过：' + r.skipped_offline)
    ElMessage.success('已创建 ' + r.tasks.length + ' 条任务')
    router.push('/tasks?batch=' + encodeURIComponent(r.batch_id))
  } catch (e) {
    ElMessage.error('创建失败：' + e.message)
  } finally {
    saving.value = false
  }
}

// 同一个路由切 query：组件不重建，靠 watch 切换“选文件 / 选设备”两种状态
watch(() => route.query.file, (v) => {
  if (v) load()
  else loadPick()
})

onMounted(() => {
  if (fileName.value) load()
  else loadPick()
})
</script>

<style scoped>
.back { display: flex; gap: 4px; margin-bottom: 10px; }
.crumbs { display: flex; align-items: center; flex-wrap: wrap; gap: 2px; margin: -4px 0 10px; font-size: 13px; }
.crumbs .sep { color: var(--el-text-color-placeholder); }
.fname { display: inline-flex; align-items: center; gap: 6px; }
.ico { margin-right: 4px; vertical-align: -2px; color: var(--el-text-color-secondary); }
.cfg { display: flex; flex-wrap: wrap; gap: 10px 16px; align-items: flex-end; margin-bottom: 12px; }
.cfg-item { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--el-text-color-secondary); }
.w220 { width: 200px; }
.foot { display: flex; align-items: center; gap: 10px; margin-top: 12px; flex-wrap: wrap; }
.foot .spacer { flex: 1; }
@media (max-width: 599px) {
  .w220 { width: 100%; }
  .cfg-item { flex: 1 1 100%; }
  .cfg-item :deep(.el-date-editor) { width: 100%; }
}
</style>
