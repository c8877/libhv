<template>
  <div class="panel">
    <div class="panel-head">
      <h2>串口调试 · {{ alias }}</h2>
      <el-tag :type="stateType" size="small">{{ stateText }}</el-tag>
      <span v-if="monitor" class="ro">只读监听</span>
      <span class="spacer" />
      <el-button v-if="captureFile" size="small" @click="downloadCapture">下载抓流</el-button>
      <el-button size="small" :disabled="state !== 'open'" @click="disconnect">断开</el-button>
      <el-button size="small" @click="$router.push('/agents/' + encodeURIComponent(devid))">返回设备</el-button>
    </div>

    <div ref="logEl" class="tty-console">
      <div v-for="(e, i) in entries" :key="i" class="tty-line">
        <span v-if="showTs" class="tty-ts">{{ tsText(e.ts) }}</span>
        <span v-if="e.dir === 'sys'" class="tty-sys">!</span>
        <span v-else :class="e.dir === 'rx' ? 'tty-rx' : 'tty-tx'">{{ e.dir === 'rx' ? '←' : '→' }}</span>
        <template v-if="e.dir === 'sys'"><span class="tty-text">{{ e.text }}</span></template>
        <template v-else>
          <span v-if="showHex" class="tty-hex">{{ e.hex }}</span>
          <span v-if="showText" class="tty-text">{{ e.txt }}</span>
        </template>
      </div>
      <div v-if="!entries.length" class="muted">等待数据…</div>
    </div>

    <div class="tty-send">
      <el-select v-model="mode" :disabled="monitor" class="w90">
        <el-option label="文本" value="text" />
        <el-option label="HEX" value="hex" />
      </el-select>
      <el-select v-model="eol" :disabled="monitor" class="w110">
        <el-option label="无行尾" value="none" />
        <el-option label="CRLF" value="crlf" />
        <el-option label="LF" value="lf" />
        <el-option label="CR" value="cr" />
      </el-select>
      <el-input v-model="input" :disabled="monitor" placeholder="输入后 Ctrl+Enter 发送"
                @keydown.ctrl.enter.prevent="send" @keydown.meta.enter.prevent="send" />
      <el-button type="primary" :disabled="monitor || state !== 'open'" @click="send">发送</el-button>
    </div>

    <div class="tty-tools">
      <el-checkbox v-model="showTs">时间戳</el-checkbox>
      <el-checkbox v-model="showHex">HEX</el-checkbox>
      <el-checkbox v-model="showText">文本</el-checkbox>
      <el-checkbox v-model="paused" @change="onPause">暂停</el-checkbox>
      <el-button size="small" @click="clear">清空</el-button>
      <el-button size="small" @click="saveLog">保存日志</el-button>
      <span class="muted">共 {{ entries.length }} 行</span>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'

const route = useRoute()
const devid = decodeURIComponent(route.params.devid || '')
const alias = decodeURIComponent(route.params.alias || '')
const monitor = route.query.monitor === '1'
const takeover = route.query.takeover === '1'

const MAX_LINES = 2000
const entries = ref([])
const pending = ref([])
const input = ref('')
const mode = ref('text')
const eol = ref('none')
const EOLS = { none: '', crlf: '\r\n', lf: '\n', cr: '\r' }
const showTs = ref(true)
const showHex = ref(true)
const showText = ref(true)
const paused = ref(false)
const state = ref('connecting')
const captureFile = ref('')
const logEl = ref(null)
let ws = null

const stateText = computed(() => ({
  connecting: '连接中…', open: '已连接', closed: '已断开', error: '连接失败',
}[state.value] || '连接中…'))
const stateType = computed(() => ({
  connecting: 'info', open: 'success', closed: 'info', error: 'danger',
}[state.value] || 'info'))

function tsText(ts) {
  const d = new Date(ts)
  const p = (n) => String(n).padStart(2, '0')
  return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) +
    '.' + String(ts % 1000).padStart(3, '0')
}

function fmtHex(bytes) {
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join(' ')
}

function fmtText(bytes) {
  if (!bytes) return ''
  return new TextDecoder('utf-8', { fatal: false }).decode(bytes)
    .replace(/\r/g, '\\r').replace(/\n/g, '\\n')
}

function trim() {
  while (entries.value.length > MAX_LINES) entries.value.shift()
}

function addEntry(dir, bytes) {
  const e = dir === 'sys'
    ? { ts: Date.now(), dir, text: String(bytes) }
    : { ts: Date.now(), dir, hex: fmtHex(bytes), txt: fmtText(bytes) }
  if (paused.value) {
    pending.value.push(e)
    while (pending.value.length > MAX_LINES) pending.value.shift()
    return
  }
  entries.value.push(e)
  trim()
}

function onPause(v) {
  if (!v && pending.value.length) {
    entries.value.push(...pending.value)
    pending.value = []
    trim()
  }
}

function scrollToEnd() {
  nextTick(() => {
    if (logEl.value) logEl.value.scrollTop = logEl.value.scrollHeight
  })
}

watch(() => entries.value.length, scrollToEnd)

function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  let url = proto + '//' + location.host + '/ws/tty/' +
    encodeURIComponent(devid) + '/' + encodeURIComponent(alias)
  const q = []
  if (monitor) q.push('monitor=1')
  if (takeover) q.push('takeover=1')
  if (q.length) url += '?' + q.join('&')
  state.value = 'connecting'
  const sock = new WebSocket(url)
  sock.binaryType = 'arraybuffer'
  ws = sock
  sock.onopen = () => { state.value = 'open' }
  sock.onclose = () => { if (ws === sock) ws = null; state.value = 'closed' }
  sock.onerror = () => { state.value = 'error' }
  sock.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      try {
        const j = JSON.parse(ev.data)
        if (j.op === 'error') addEntry('sys', j.msg || 'error')
        if (j.op === 'open_ok' && j.file) captureFile.value = j.file
      } catch (e) { /* 非 JSON 文本 */ }
      return
    }
    addEntry('rx', new Uint8Array(ev.data))
  }
}

function send() {
  if (!ws || ws.readyState !== 1) return
  const raw = input.value
  let bytes
  if (mode.value === 'hex') {
    const clean = raw.replace(/[^0-9a-fA-F]/g, '')
    bytes = new Uint8Array(Math.floor(clean.length / 2))
    for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(clean.substr(i * 2, 2), 16)
  } else {
    bytes = new TextEncoder().encode(raw + (EOLS[eol.value] || ''))
  }
  if (!bytes.length) return
  let bin = ''
  bytes.forEach((b) => { bin += String.fromCharCode(b) })
  ws.send(JSON.stringify({ op: 'write', data: btoa(bin) }))
  addEntry('tx', bytes)
  input.value = ''
}

function disconnect() {
  if (ws) ws.close()
}

function clear() {
  entries.value = []
  pending.value = []
}

function downloadCapture() {
  location.href = '/api/v1/ttylogs/' + encodeURIComponent(captureFile.value)
}

function saveLog() {
  const lines = entries.value.map((e) => {
    if (e.dir === 'sys') return '[' + new Date(e.ts).toISOString() + '] SYS ' + (e.text || '')
    return '[' + new Date(e.ts).toISOString() + '] ' + e.dir.toUpperCase() + ' ' + e.hex
  })
  const safe = alias.replace(/[^A-Za-z0-9_.-]/g, '_')
  const blob = new Blob([lines.join('\n')], { type: 'text/plain' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = devid + '-' + safe + '-tty.log'
  a.click()
  URL.revokeObjectURL(a.href)
}

onMounted(connect)
onBeforeUnmount(() => {
  if (ws) {
    ws.onclose = null
    ws.close()
    ws = null
  }
})
</script>

<style scoped>
.ro {
  font-size: 12px;
  color: var(--el-color-warning);
  border: 1px solid var(--el-color-warning-light-5);
  border-radius: 6px;
  padding: 1px 8px;
}
.tty-console {
  height: 420px;
  overflow-y: auto;
  background: #14161a;
  border: 1px solid var(--el-border-color-darker);
  border-radius: 8px;
  padding: 10px 12px;
  font-family: ui-monospace, Menlo, Consolas, monospace;
  font-size: 12px;
  line-height: 1.7;
  color: #d7dce3;
}
.tty-line { white-space: pre-wrap; word-break: break-all; }
.tty-ts { color: #6f7885; margin-right: 6px; }
.tty-rx { color: #57ab5a; }
.tty-tx { color: #4a9eff; }
.tty-sys { color: #e5534b; }
.tty-hex { color: #9aa4b2; }
.tty-text { color: #d7dce3; margin-left: 8px; }
.tty-send { display: flex; gap: 8px; margin-top: 12px; }
.tty-send .w90 { width: 90px; flex: none; }
.tty-send .w110 { width: 110px; flex: none; }
.tty-tools { display: flex; align-items: center; gap: 12px; margin-top: 12px; flex-wrap: wrap; }
@media (max-width: 599px) {
  .tty-console { height: 52vh; }
  .tty-send { flex-wrap: wrap; }
  .tty-send .el-input { width: 100%; }
  .tty-send .el-button { flex: 1; }
}
</style>
