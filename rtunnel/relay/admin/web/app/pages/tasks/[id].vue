<template>
  <div v-if="!t" class="panel">
    <el-skeleton :rows="4" animated />
  </div>
  <div v-else>
    <div class="back">
      <el-button link @click="$router.push('/tasks')">← 任务列表</el-button>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>任务 #{{ t.id }}</h2>
        <el-tag :type="statusType(t.status)" size="small">{{ statusText(t.status) }}</el-tag>
        <span class="spacer" />
        <el-button v-if="t.status === 'failed'" type="primary" @click="retry">重试</el-button>
        <el-button v-if="t.status === 'scheduled' || t.status === 'queued'" @click="cancel">取消</el-button>
        <el-button v-if="t.status === 'timeout' && t.extend_count === 0" @click="extend">延长截止</el-button>
        <el-button v-if="t.status === 'conflict'" @click="check('verify')">校验设备</el-button>
        <el-button v-if="t.status === 'conflict'" @click="check('force')">强制重发</el-button>
        <el-button @click="$router.push('/tasks')">返回列表</el-button>
      </div>

      <div class="file-line">
        <b>{{ t.file_name }}</b>
        <span class="muted">{{ bytes(t.file_size) }}</span>
        <template v-if="t.file_sha">
          <code class="mono">{{ t.file_sha }}</code>
          <el-button link type="primary" @click="copy(t.file_sha)">复制哈希</el-button>
        </template>
      </div>

      <el-progress :percentage="pct" :stroke-width="10" :status="progressStatus" />
      <div class="stats muted">{{ bytes(t.sent) }} / {{ bytes(t.file_size) }} · {{ pct }}%</div>
    </div>

    <div class="grid-2">
      <div class="panel">
        <div class="panel-head"><h2>传输</h2></div>
        <el-descriptions :column="1" size="small" border>
          <el-descriptions-item label="设备">{{ t.devid }}</el-descriptions-item>
          <el-descriptions-item label="目标路径">{{ t.target_path || '—' }}</el-descriptions-item>
          <el-descriptions-item label="重试">{{ t.retry_count }} / {{ t.max_retries }}</el-descriptions-item>
          <el-descriptions-item label="批次">{{ t.batch_id || '—' }}</el-descriptions-item>
        </el-descriptions>
      </div>
      <div class="panel">
        <div class="panel-head"><h2>时间与操作</h2></div>
        <el-descriptions :column="1" size="small" border>
          <el-descriptions-item label="创建时间">{{ time(t.created_ms) }}</el-descriptions-item>
          <el-descriptions-item label="开始时间">{{ time(t.start_at_ms) }}</el-descriptions-item>
          <el-descriptions-item label="截止时间">{{ time(t.deadline_at_ms) }}</el-descriptions-item>
          <el-descriptions-item label="操作人">{{ t.actor || '—' }}</el-descriptions-item>
        </el-descriptions>
      </div>
    </div>

    <el-alert v-if="t.error" type="error" :title="'错误：' + t.error" :closable="false" show-icon />
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '~/shared/api.js'
import { bytes, time } from '~/shared/utils.js'

const route = useRoute()
const t = ref(null)

const STATUS = {
  scheduled: ['等待开始', 'warning'], queued: ['排队中', 'warning'], running: ['传输中', 'warning'],
  success: ['成功', 'success'], failed: ['失败', 'danger'], timeout: ['超时', 'danger'],
  canceled: ['已取消', 'info'], conflict: ['冲突', 'warning'],
}
function statusText(s) { return (STATUS[s] || ['未知', 'warning'])[0] }
function statusType(s) { return (STATUS[s] || ['未知', 'warning'])[1] }

const pct = computed(() => {
  const size = Number(t.value && t.value.file_size) || 0
  const sent = Number(t.value && t.value.sent) || 0
  return size ? Math.min(100, Math.floor((sent * 100) / size)) : 0
})
const progressStatus = computed(() => {
  if (!t.value) return ''
  if (t.value.status === 'success') return 'success'
  if (['failed', 'timeout', 'conflict'].includes(t.value.status)) return 'exception'
  return ''
})

async function load(silent) {
  try {
    t.value = await Api.get('/api/v1/distributions/' + route.params.id)
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  }
}

async function retry() {
  try {
    await Api.post('/api/v1/distributions/' + route.params.id + '/retry', {})
    ElMessage.success('已重试')
    await load(true)
  } catch (e) { ElMessage.error('重试失败：' + e.message) }
}

async function cancel() {
  try {
    await Api.post('/api/v1/distributions/' + route.params.id + '/cancel', {})
    ElMessage.success('已取消')
    await load(true)
  } catch (e) { ElMessage.error('取消失败：' + e.message) }
}

async function extend() {
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
    await Api.post('/api/v1/distributions/' + route.params.id + '/extend',
      { deadline_at_ms: Date.now() + hours * 3600 * 1000 })
    ElMessage.success('已延长')
    await load(true)
  } catch (e) { ElMessage.error('延长失败：' + e.message) }
}

async function check(api) {
  try {
    const r = await Api.post('/api/v1/distributions/' + route.params.id + '/' + api, {})
    ElMessage.success(r.same ? '设备文件一致，已标记成功' : '设备文件不一致，已重新排队')
    await load(true)
  } catch (e) { ElMessage.error('操作失败：' + e.message) }
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('哈希已复制')
  } catch (e) { ElMessage.warning('复制失败，请手动选择') }
}

let timer = null
onMounted(() => {
  load()
  timer = setInterval(() => { if (!document.hidden) load(true) }, 3000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
watch(() => route.params.id, () => load())
</script>

<style scoped>
.back { margin-bottom: 10px; }
.file-line { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; flex-wrap: wrap; }
.file-line code { font-size: 12px; color: var(--el-text-color-secondary); word-break: break-all; }
.stats { margin-top: 6px; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; gap: 8px; }
}
</style>
