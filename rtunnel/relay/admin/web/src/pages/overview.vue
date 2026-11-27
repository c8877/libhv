<template>
  <div>
    <!-- 待处理：概览先回答“现在有没有事要做”，再回答“有多少” -->
    <div v-if="todoTiles.length" class="panel todo">
      <div class="panel-head">
        <h2>待处理</h2>
        <el-tag v-if="todoTotal === 0" type="success" size="small" effect="plain">全部处理完</el-tag>
        <span v-else class="muted">{{ todoTotal }} 项需要看一眼</span>
        <span class="spacer" />
        <span class="muted">15s 自动刷新</span>
      </div>
      <div class="tiles">
        <router-link v-for="t in todoTiles" :key="t.key" :to="t.to" class="tile"
                     :class="{ hot: t.value > 0 }">
          <div class="tile-num">{{ t.value }}</div>
          <div class="tile-label">{{ t.label }}</div>
          <div class="tile-hint">{{ t.value > 0 ? t.hint : t.zero }}</div>
        </router-link>
      </div>
    </div>

    <div class="cards">
      <router-link v-for="s in stats" :key="s.label" :to="s.to" class="stat">
        <div class="num">{{ s.value }}</div>
        <div class="label">{{ s.label }}</div>
      </router-link>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>运行信息</h2>
        <span class="spacer" />
        <el-button v-if="can('ai.chat')" size="small" @click="diagnose">AI 诊断</el-button>
        <span class="muted">5s 自动刷新</span>
      </div>
      <el-descriptions v-if="o" :column="2" border>
        <el-descriptions-item label="版本">{{ o.version }}</el-descriptions-item>
        <el-descriptions-item label="运行时长">{{ duration(o.uptime_ms) }}</el-descriptions-item>
        <el-descriptions-item label="mTLS">
          <el-tag :type="o.mtls ? 'success' : 'warning'" size="small">{{ o.mtls ? '开启' : '关闭' }}</el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="ACL">
          <el-tag :type="o.allow_all ? 'warning' : 'primary'" size="small">
            {{ o.allow_all ? 'allow-all' : '按表授权' }}
          </el-tag>
        </el-descriptions-item>
      </el-descriptions>
      <el-skeleton v-else :rows="2" animated />
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>实时事件</h2>
        <el-tag size="small" :type="sseState === 'open' ? 'success' : (sseState === 'connecting' ? 'info' : 'warning')">
          {{ sseText }}
        </el-tag>
        <span class="spacer" />
        <span class="muted">新事件在上</span>
        <el-button text size="small" @click="events = []">清空</el-button>
      </div>
      <el-scrollbar height="240px">
        <div v-for="e in events" :key="e.seq" class="evt">
          <span class="evt-time">{{ time(e.ts) }}</span>
          <el-tag size="small" :type="tagType(e.type)" effect="plain">{{ label(e.type) }}</el-tag>
          <span class="evt-text">{{ text(e) }}</span>
        </div>
        <div v-if="!events.length" class="muted evt-empty">
          等待事件…（设备上下线、会话、审计、告警、任务都会出现在这里）
        </div>
      </el-scrollbar>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can, overview } from '@/shared/store.js'
import { useRouter } from 'vue-router'
import { bytes, duration, time } from '@/shared/utils.js'

const o = ref(overview.data)
const router = useRouter()

// ---------------------------------------------------------------- 待处理
const todo = ref({ alerts: 0, pending: 0, failed: 0, certs: 0 })

const todoTiles = computed(() => {
  const t = todo.value
  const out = []
  if (can('alert.read')) {
    out.push({ key: 'alerts', value: t.alerts, label: '未确认告警', to: '/alerts',
      hint: '设备离线 / 证书到期 / 任务失败', zero: '没有未确认的告警' })
  }
  if (can('device.exec')) {
    out.push({ key: 'pending', value: t.pending, label: '待审批作业', to: '/operations',
      hint: '高风险命令等你批准', zero: '没有待审批的命令' })
  }
  if (can('dist.read')) {
    out.push({ key: 'failed', value: t.failed, label: '失败任务', to: '/tasks',
      hint: '分发 / OTA 需要重试', zero: '没有失败的任务' })
  }
  if (can('cert.read')) {
    out.push({ key: 'certs', value: t.certs, label: '证书将过期', to: '/certs',
      hint: '30 天内到期，建议续签', zero: '证书都在有效期内' })
  }
  return out
})

const todoTotal = computed(() => todoTiles.value.reduce((n, t) => n + (t.value || 0), 0))

async function loadTodo() {
  const [al, ops, dist, certs] = await Promise.all([
    can('alert.read') ? Api.get('/api/v1/alerts?state=open&limit=1').catch(() => null) : null,
    can('device.exec') ? Api.get('/api/v1/operations?status=pending_approval&limit=1').catch(() => null) : null,
    can('dist.read') ? Api.get('/api/v1/distributions/summary').catch(() => null) : null,
    can('cert.read') ? Api.get('/api/v1/certs').catch(() => null) : null,
  ])
  todo.value = {
    alerts: (al && al.counts && al.counts.open) || 0,
    pending: (ops && ops.total) || 0,
    failed: (dist && dist.failed) || 0,
    certs: Array.isArray(certs) ? certs.filter((c) => c.status === 'expiring').length : 0,
  }
}

const stats = computed(() => {
  const d = o.value || {}
  return [
    { label: '在线设备', value: d.agents ?? '—', to: '/agents' },
    { label: '在线客户端', value: d.clients ?? '—', to: '/clients' },
    { label: '活跃会话', value: d.streams ?? '—', to: '/streams' },
    { label: '累计下行', value: d.bytes_in != null ? bytes(d.bytes_in) : '—', to: '/usage' },
    { label: '累计上行', value: d.bytes_out != null ? bytes(d.bytes_out) : '—', to: '/usage' },
  ]
})

let timer = null
async function pull() {
  try {
    overview.data = await Api.get('/api/v1/overview')
    o.value = overview.data
  } catch (e) { /* 保留上次数据 */ }
}

// ---------------------------------------------------------------- 实时事件
// SSE 用浏览器 Cookie 鉴权（管理台会话），断线由 EventSource 自动重连，
// 服务端按 Last-Event-ID 续传，不会丢事件。
const events = ref([])
const sseState = ref('connecting')
let es = null

const sseText = computed(() => ({
  open: '已连接', connecting: '连接中', error: '已断开，重试中',
}[sseState.value] || sseState.value))

const LABELS = {
  'device.online': '上线', 'device.offline': '离线',
  'stream.open': '会话建立', 'stream.close': '会话结束',
  audit: '审计', alert: '告警',
  'task.success': '任务成功', 'task.failed': '任务失败',
  'bus.dropped': '事件丢弃', 'bus.lost': '事件空洞',
}
function label(t) { return LABELS[t] || t }
function tagType(t) {
  if (t === 'device.offline' || t === 'task.failed' || t === 'alert') return 'danger'
  if (t === 'device.online' || t === 'task.success') return 'success'
  if (t === 'stream.open') return 'primary'
  return 'info'
}
function text(e) {
  const d = e.data || {}
  if (e.type === 'stream.open' || e.type === 'stream.close') {
    const parts = [d.devid, d.kind, d.port ? ':' + d.port : '']
    if (e.type === 'stream.close') {
      if (d.bytes_down != null) parts.push('↓' + bytes(d.bytes_down))
      if (d.duration_ms != null) parts.push(duration(d.duration_ms))
    }
    return parts.filter(Boolean).join(' ')
  }
  if (e.type === 'audit') {
    return [d.actor, d.action, d.target_id, d.detail].filter(Boolean).join(' · ')
  }
  if (e.type === 'alert') return [d.level, d.title, d.text].filter(Boolean).join(' · ')
  if (e.type.startsWith('task.')) {
    return ['任务#' + d.task_id, d.devid, d.kind, d.status, d.error].filter(Boolean).join(' ')
  }
  if (d.devid) return [d.devid, d.version, d.error].filter(Boolean).join(' ')
  return JSON.stringify(d)
}

function onEvent(ev) {
  let data = {}
  try { data = JSON.parse(ev.data || '{}') } catch (e) { data = {} }
  events.value.unshift({
    seq: ev.lastEventId ? Number(ev.lastEventId) : Date.now(),
    type: ev.type,
    ts: data.ts || Date.now(),
    data,
  })
  if (events.value.length > 100) events.value.length = 100
}

function connect() {
  if (typeof EventSource === 'undefined') { sseState.value = 'error'; return }
  es = new EventSource('/api/v1/events')
  es.onopen = () => { sseState.value = 'open' }
  es.onerror = () => { sseState.value = 'error' }   // 浏览器会自己重连
  es.addEventListener('hello', () => { sseState.value = 'open' })   // hello 只用来确认连通
  for (const t of Object.keys(LABELS)) {
    es.addEventListener(t, onEvent)
  }
}

// 一键巡检：跳到首页的助手，带着问题直接发问
function diagnose() {
  const q = '请做一次平台巡检：1) 平台总览（在线设备/客户端/会话、告警状态）；' +
    '2) 指出离线设备、版本落后或负载/内存偏高的设备；' +
    '3) 汇总失败的分发/OTA 任务、最近的自愈触发与快过期证书；' +
    '4) 按优先级给出下一步排查建议。只依据工具数据，不要猜测。'
  router.push({ path: '/', query: { q } })
}

let todoTimer = null
onMounted(() => {
  pull()
  loadTodo()
  timer = setInterval(pull, 5000)
  todoTimer = setInterval(loadTodo, 15000)
  connect()
})
onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
  if (todoTimer) clearInterval(todoTimer)
  if (es) { es.close(); es = null }
})
</script>

<style scoped>
/* 待处理 */
.tiles {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: 12px;
}
.tile {
  display: block;
  padding: 14px 16px;
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 10px;
  background: var(--el-fill-color-light);
  text-decoration: none;
  transition: border-color 0.15s ease, background 0.15s ease;
}
.tile:hover { border-color: var(--el-color-primary-light-5); }
.tile.hot { background: color-mix(in srgb, var(--el-color-danger) 6%, var(--el-bg-color)); border-color: color-mix(in srgb, var(--el-color-danger) 26%, transparent); }
.tile-num {
  font-size: 24px;
  font-weight: 700;
  line-height: 1.2;
  font-variant-numeric: tabular-nums;
  color: var(--el-text-color-primary);
}
.tile.hot .tile-num { color: var(--el-color-danger); }
.tile-label { margin-top: 2px; font-size: 13px; color: var(--el-text-color-regular); }
.tile-hint { margin-top: 4px; font-size: 12px; color: var(--el-text-color-secondary); }

.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  gap: 14px;
  margin-bottom: 18px;
}
.stat {
  position: relative;
  overflow: hidden;
  display: block;
  text-decoration: none;
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color-lighter);
  border-radius: var(--app-radius);
  box-shadow: var(--app-shadow);
  padding: 18px 18px 16px;
  transition: transform 0.18s ease, box-shadow 0.18s ease;
}
.stat::before {
  content: "";
  position: absolute;
  left: 0;
  top: 0;
  width: 30px;
  height: 3px;
  border-radius: 0 0 4px 0;
  background: var(--el-color-primary);
  opacity: 0.85;
}
.stat:hover {
  transform: translateY(-1px);
  box-shadow: 0 2px 4px rgba(15, 23, 42, 0.05), 0 20px 44px -26px rgba(15, 23, 42, 0.4);
}
.num {
  font-size: 28px;
  font-weight: 700;
  line-height: 1.15;
  letter-spacing: -0.02em;
  font-variant-numeric: tabular-nums;
  color: var(--el-text-color-primary);
}
.label {
  margin-top: 6px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}
.evt {
  display: flex;
  align-items: baseline;
  gap: 8px;
  padding: 5px 2px;
  border-bottom: 1px dashed var(--el-border-color-lighter);
  font-size: 12.5px;
}
.evt-time { color: var(--el-text-color-secondary); font-variant-numeric: tabular-nums; }
.evt-text { word-break: break-all; }
.evt-empty { padding: 14px 2px; }
</style>
