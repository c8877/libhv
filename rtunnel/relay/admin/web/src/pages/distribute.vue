<template>
  <div v-if="!fileName" class="panel">
    <el-empty description="缺少文件参数">
      <el-button type="primary" @click="$router.replace('/files')">返回文件列表</el-button>
    </el-empty>
  </div>

  <div v-else>
    <div class="back">
      <el-button link @click="$router.push('/files')">← 文件列表</el-button>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>分发文件：{{ fileName }}</h2>
        <span class="spacer" />
        <span class="muted">已选 {{ selected.size }} / 在线 {{ onlineCount }} / 共 {{ devices.length }}</span>
      </div>

      <div class="cfg">
        <el-input v-model="q" placeholder="搜索设备 / 归属" clearable class="w220">
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
                @selection-change="onSelect">
        <el-table-column type="selection" width="46" reserve-selection :selectable="(r) => r.online" />
        <el-table-column prop="devid" label="设备" min-width="140">
          <template #default="{ row }">
            {{ row.devid }}
            <div v-if="isPhone && row.owner" class="muted">{{ row.owner }}</div>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="row.online ? 'success' : 'info'" size="small">{{ row.online ? '在线' : '离线' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="owner" label="归属" min-width="120">
          <template #default="{ row }">{{ row.owner || '—' }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150">
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
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const route = useRoute()
const router = useRouter()
const { isPhone } = useBreakpoint()
const fileName = computed(() => String(route.query.file || ''))

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
  return devices.value.filter((d) =>
    String(d.devid).toLowerCase().includes(k) || String(d.owner || '').toLowerCase().includes(k))
})
const pageRows = computed(() => filtered.value.slice((page.value - 1) * per, page.value * per))

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
      devid: a.devid, online: true, remote_addr: a.remote_addr || '', owner: '',
    }))
    ;(devs || []).forEach((d) => {
      if (!map.has(d.devid)) map.set(d.devid, { devid: d.devid, online: false, remote_addr: '', owner: d.owner || '' })
      else map.get(d.devid).owner = d.owner || ''
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

onMounted(load)
</script>

<style scoped>
.back { margin-bottom: 10px; }
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
