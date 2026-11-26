<template>
  <div class="chat-page">
    <div class="panel chat-panel">
      <div class="panel-head">
        <h2>AI 助手</h2>
        <el-tag v-if="cfg" size="small" :type="cfg.key_set ? 'success' : 'warning'">
          {{ cfg.model || '未配置' }}
        </el-tag>
        <el-tag v-if="cfg && !cfg.enabled" size="small" type="info">已停用</el-tag>
        <span class="spacer" />
        <el-select v-model="devid" clearable filterable placeholder="设备上下文（可选）"
                   style="width: 210px" @change="onDevChange">
          <el-option v-for="d in devices" :key="d.devid" :label="d.devid" :value="d.devid">
            <span>{{ d.devid }}</span>
            <el-tag v-if="!d.online" size="small" type="info" class="ml6">离线</el-tag>
          </el-option>
        </el-select>
        <el-button text :disabled="!msgs.length" @click="reset">新对话</el-button>
        <el-button v-if="canConfig" text @click="$router.push('/settings')">模型配置</el-button>
      </div>

      <div v-if="!cfg || !cfg.enabled || !cfg.key_set" class="setup">
        <el-alert type="warning" :closable="false" show-icon>
          <template #title>
            {{ !cfg ? '读不到模型配置' : (!cfg.enabled ? 'AI 助手已停用' : '还没有配置 API 密钥') }}
          </template>
          <div class="setup-text">
            到「系统设置 → AI 模型」填 endpoint / 模型名，并保存密钥，然后回来提问。
            <span v-if="!cfg || !cfg.enabled">开关也要打开。</span>
          </div>
          <el-button v-if="canConfig" size="small" @click="$router.push('/settings')">去配置</el-button>
        </el-alert>
      </div>

      <el-scrollbar ref="scroller" class="msgs" height="calc(100vh - 320px)">
        <div v-if="!msgs.length" class="empty">
          <div class="empty-title">我可以帮你查这套系统里的实际情况</div>
          <div class="chips">
            <div v-for="q in SUGGEST" :key="q" class="chip" @click="ask(q)">{{ q }}</div>
          </div>
          <div class="muted empty-tip">
            读数据用只读工具；你要它改东西时，它会生成一张待确认作业卡（不会自己执行）。
          </div>
        </div>

        <div v-for="(m, i) in msgs" :key="i" class="msg" :class="m.role">
          <div class="bubble">
            <div v-if="m.role === 'assistant'" class="md" v-html="render(m.content)" />
            <div v-else class="plain">{{ m.content }}</div>

            <div v-if="m.actions && m.actions.length" class="acts">
              <div v-for="(a, k) in m.actions" :key="k" class="act">
                <div class="act-head">
                  <el-tag size="small" :type="riskType(a.risk)" effect="plain">风险 {{ a.risk }}</el-tag>
                  <span class="act-title">{{ a.type === 'propose_exec' ? '建议执行' : '建议自愈规则' }}</span>
                  <span class="muted">{{ a.type === 'propose_exec' ? a.devid : (a.draft || {}).devid_glob }}</span>
                  <span class="spacer" />
                  <el-tag v-if="a.type === 'propose_exec'" size="small"
                          :type="a.status === 'applied' || a.status === 'verified' ? 'success' : (a.need_approval ? 'warning' : 'info')">
                    {{ opText(a) }}
                  </el-tag>
                  <el-tag v-else size="small" type="info">草稿</el-tag>
                </div>
                <div class="act-cmd">
                  <code>{{ cmdText(a) }}</code>
                </div>
                <div v-if="a.reason || (a.draft && a.draft.note)" class="muted act-reason">
                  {{ a.reason || a.draft.note }}
                </div>
                <div class="act-ops">
                  <template v-if="a.type === 'propose_exec'">
                    <el-button v-if="a.need_approval" size="small" :loading="a._busy" @click="approveOp(a)">
                      批准
                    </el-button>
                    <el-button size="small" type="primary" :loading="a._busy"
                               :disabled="a.need_approval || a.status === 'applied' || a.status === 'verified'"
                               @click="runOp(a)">执行</el-button>
                    <el-button size="small" text @click="openOps">去作业页</el-button>
                  </template>
                  <template v-else>
                    <el-button size="small" type="primary" :loading="a._busy" @click="createRule(a)">
                      创建为停用规则
                    </el-button>
                    <el-button size="small" text @click="$router.push('/playbooks')">去自愈页</el-button>
                  </template>
                </div>
                <pre v-if="a._result" class="act-result">{{ a._result }}</pre>
              </div>
            </div>

            <div v-if="m.tools && m.tools.length" class="tools">
              <div v-for="(t, j) in m.tools" :key="j" class="tool">
                <el-tag size="small" :type="t.ok ? 'info' : 'danger'" effect="plain">
                  {{ t.name }}
                </el-tag>
                <span class="muted tool-args">{{ argsText(t.args) }}</span>
                <el-tag v-if="!t.ok" size="small" type="danger" effect="plain">失败</el-tag>
              </div>
            </div>
            <div v-if="m.error" class="err">{{ m.error }}</div>
            <div v-if="m.meta" class="muted meta">{{ m.meta }}</div>
          </div>
        </div>

        <div v-if="busy" class="msg assistant">
          <div class="bubble thinking">
            <span class="dot" /><span class="dot" /><span class="dot" />
            <span class="muted">正在查数据…</span>
          </div>
        </div>
      </el-scrollbar>

      <div class="composer">
        <el-input v-model="input" type="textarea" :rows="2" resize="none" maxlength="2000"
                  :placeholder="devid ? ('问关于 ' + devid + ' 的问题，Enter 发送，Shift+Enter 换行') : '问点什么，Enter 发送，Shift+Enter 换行'"
                  @keydown="onKey" />
        <el-button type="primary" :loading="busy" :disabled="!canSend" @click="send">发送</el-button>
      </div>
      <div class="muted foot">
        回答由模型生成，数据来自本平台的只读接口；涉及变更请走「作业」并走审批。
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, ref } from 'vue'
import { useRoute } from 'vue-router'
import { ElMessage } from 'element-plus'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'

const route = useRoute()

const SUGGEST = [
  '现在有哪些设备在线，哪台离线了？',
  '最近 6 小时有没有设备的负载异常升高？',
  '最近谁改过权限或接入账号？',
  '有没有失败的作业或自愈规则触发？',
  '这个月的流量和会话用量大概多少？',
]

const cfg = ref(null)
const devices = ref([])
const devid = ref('')
const msgs = ref([])
const input = ref('')
const busy = ref(false)
const scroller = ref(null)
const canConfig = computed(() => can('ai.config'))
const canSend = computed(() => !busy.value && input.value.trim().length > 0 &&
  cfg.value && cfg.value.enabled && cfg.value.key_set)

function argsText(a) {
  if (!a || typeof a !== 'object') return ''
  const s = Object.entries(a).map(([k, v]) => k + '=' + v).join(' ')
  return s.length > 60 ? s.slice(0, 60) + '…' : s
}

// 极简渲染：只处理 **粗体**、`代码`、换行，其余按文本转义，避免 v-html 注入
function render(text) {
  const esc = String(text || '').replace(/[&<>"']/g, (c) => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]))
  return esc
    .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\n/g, '<br>')
}

async function scrollBottom() {
  await nextTick()
  const el = scroller.value && scroller.value.wrapRef
  if (el) el.scrollTop = el.scrollHeight
}

async function loadCfg() {
  try {
    cfg.value = await Api.get('/api/v1/ai/config')
  } catch (e) {
    cfg.value = null
  }
}

async function loadDevices() {
  try {
    const r = await Api.get('/api/v1/agents')
    const list = Array.isArray(r) ? r : (r.items || [])
    devices.value = list.map((d) => ({ devid: d.devid, online: !!d.online }))
  } catch (e) {
    devices.value = []
  }
}

function onDevChange() {
  if (devid.value) input.value = input.value || ('看下 ' + devid.value + ' 现在的状态')
}

function reset() {
  msgs.value = []
  input.value = ''
}

function ask(q) {
  input.value = q
  send()
}

function onKey(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault()
    send()
  }
}

async function send() {
  const text = input.value.trim()
  if (!text || busy.value) return
  if (!cfg.value || !cfg.value.enabled || !cfg.value.key_set) {
    ElMessage.warning('先把模型配置和密钥补全')
    return
  }
  msgs.value.push({ role: 'user', content: text })
  input.value = ''
  busy.value = true
  scrollBottom()
  // 只发最近 20 条，避免上下文过长
  const history = msgs.value.slice(-20).map((m) => ({ role: m.role, content: m.content }))
  try {
    const r = await Api.post('/api/v1/ai/chat', { messages: history, devid: devid.value || undefined })
    const meta = []
    if (r.rounds) meta.push(r.rounds + ' 轮')
    if (r.usage && r.usage.completion_tokens) meta.push(r.usage.completion_tokens + ' tokens')
    msgs.value.push({
      role: 'assistant',
      content: r.reply || '',
      actions: r.actions || [],
      tools: r.tools || [],
      meta: meta.join(' · '),
    })
  } catch (e) {
    msgs.value.push({
      role: 'assistant',
      content: '',
      error: e.message || '请求失败',
    })
  } finally {
    busy.value = false
    scrollBottom()
  }
}

// ---------------------------------------------------------------- 提议的作业/规则
// 模型只提议，执行必须人点：高风险作业先“批准”（另一个身份）再“执行”。
function riskType(r) {
  return r >= 3 ? 'danger' : (r === 2 ? 'warning' : 'info')
}
function opText(a) {
  if (a.status === 'applied' || a.status === 'verified') return '已执行'
  if (a.status === 'pending_approval') return '待批准'
  if (a.status === 'approved') return '已批准'
  if (a.status === 'rejected') return '已拒绝'
  return '待执行'
}
function cmdText(a) {
  if (a.type === 'propose_exec') return (a.argv || []).join(' ')
  const d = a.draft || {}
  const v = (d.verify_argv || []).length ? ' → 复核 ' + d.verify_argv.join(' ') : ''
  return (d.argv || []).join(' ') + v
}

async function runOp(a) {
  a._busy = true
  a._result = ''
  try {
    const r = await Api.post('/api/v1/operations/' + a.id + '/apply', { plan_hash: a.plan_hash })
    a.status = r.status || 'applied'
    a.need_approval = false
    const res = r.result || {}
    a._result = 'exit=' + (res.exit_code == null ? '?' : res.exit_code) +
      (res.stdout ? '\n' + res.stdout : '') + (res.stderr ? '\n[stderr] ' + res.stderr : '')
  } catch (e) {
    a._result = e.message || '执行失败'
  } finally {
    a._busy = false
  }
}

async function approveOp(a) {
  a._busy = true
  try {
    const r = await Api.post('/api/v1/operations/' + a.id + '/approve',
      { decision: 'approve', note: 'AI 助手内确认' })
    a.status = r.status || 'approved'
    a.need_approval = false
    a._result = '已批准，可以执行'
  } catch (e) {
    a._result = e.message || '批准失败（需要 op.approve 权限，且不能批准自己的提议）'
  } finally {
    a._busy = false
  }
}

async function createRule(a) {
  a._busy = true
  try {
    const d = a.draft || {}
    const r = await Api.post('/api/v1/playbooks', {
      name: d.name, event: d.event, devid_glob: d.devid_glob,
      argv: d.argv, verify_argv: d.verify_argv || [], note: d.note || '',
      auto_apply: false, enabled: false,
    })
    a._result = '已创建规则 #' + (r.id || '?') + '（默认停用），到「自愈」页确认后启用'
  } catch (e) {
    a._result = e.message || '创建失败'
  } finally {
    a._busy = false
  }
}

function openOps() {
  window.location.hash = '#/operations'
}

onMounted(() => {
  loadCfg()
  loadDevices()
  // 概览页的“AI 诊断”会带 ?q= 过来
  const q = route.query.q
  if (q) input.value = String(q)
})
</script>

<style scoped>
.chat-page { display: flex; justify-content: center; }
.chat-panel { width: 100%; max-width: 980px; display: flex; flex-direction: column; }
.setup { margin: 8px 0 4px; }
.setup-text { margin: 4px 0 8px; font-size: 12.5px; }
.msgs { padding: 4px 2px 8px; }
.empty { padding: 24px 8px; }
.empty-title { font-size: 15px; margin-bottom: 12px; }
.empty-tip { margin-top: 12px; font-size: 12.5px; }
.chips { display: flex; flex-wrap: wrap; gap: 8px; }
.chip {
  border: 1px solid var(--el-border-color);
  border-radius: 14px;
  padding: 6px 12px;
  font-size: 12.5px;
  cursor: pointer;
}
.chip:hover { border-color: var(--el-color-primary); color: var(--el-color-primary); }
.msg { display: flex; margin: 10px 0; }
.msg.user { justify-content: flex-end; }
.bubble {
  max-width: 82%;
  padding: 9px 12px;
  border-radius: 10px;
  background: var(--el-fill-color-light);
  font-size: 13.5px;
  line-height: 1.65;
  word-break: break-word;
}
.msg.user .bubble { background: var(--el-color-primary-light-9); }
.plain { white-space: pre-wrap; }
.md :deep(code) {
  background: var(--el-fill-color);
  padding: 1px 4px;
  border-radius: 3px;
  font-size: 12.5px;
}
.acts { margin-top: 10px; display: flex; flex-direction: column; gap: 8px; }
.act {
  border: 1px solid var(--el-border-color);
  border-left: 3px solid var(--el-color-primary);
  border-radius: 6px;
  padding: 8px 10px;
  background: var(--el-bg-color);
}
.act-head { display: flex; align-items: center; gap: 6px; font-size: 12.5px; }
.act-title { font-weight: 600; }
.act-cmd { margin-top: 6px; font-size: 12.5px; word-break: break-all; }
.act-cmd code {
  background: var(--el-fill-color);
  padding: 2px 5px;
  border-radius: 3px;
  font-family: var(--el-font-family-mono, monospace);
}
.act-reason { margin-top: 4px; font-size: 12px; }
.act-ops { margin-top: 8px; display: flex; align-items: center; gap: 6px; }
.act-result {
  margin: 8px 0 0;
  padding: 6px 8px;
  background: var(--el-fill-color-light);
  border-radius: 4px;
  font-size: 12px;
  white-space: pre-wrap;
  max-height: 160px;
  overflow: auto;
}
.tools { margin-top: 8px; display: flex; flex-direction: column; gap: 4px; }
.tool { display: flex; align-items: center; gap: 6px; font-size: 12px; }
.tool-args { font-family: var(--el-font-family-mono, monospace); }
.err { color: var(--el-color-danger); margin-top: 6px; font-size: 12.5px; }
.meta { margin-top: 6px; font-size: 11.5px; }
.thinking { display: flex; align-items: center; gap: 5px; }
.dot {
  width: 5px; height: 5px; border-radius: 50%;
  background: var(--el-text-color-secondary);
  animation: blink 1.2s infinite;
}
.dot:nth-child(2) { animation-delay: .2s; }
.dot:nth-child(3) { animation-delay: .4s; }
@keyframes blink { 0%, 100% { opacity: .25; } 50% { opacity: 1; } }
.composer { display: flex; gap: 8px; align-items: flex-end; margin-top: 8px; }
.foot { margin-top: 6px; font-size: 11.5px; }
.ml6 { margin-left: 6px; }
</style>
