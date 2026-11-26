<template>
  <div class="panel ai-chat" :class="{ compact }">
    <div class="panel-head">
      <h2 v-if="!compact">AI 助手</h2>
      <span v-else class="mini-title">AI 助手</span>
      <el-tag v-if="cfg" size="small" :type="cfg.ready ? 'success' : 'warning'">
        {{ cfg.ready ? (providerLabel(cfg.provider) + ' · ' + cfg.model) : '未配置' }}
      </el-tag>
      <span class="spacer" />
      <el-select v-model="devid" clearable filterable placeholder="设备上下文（可选）"
                 :style="{ width: compact ? '170px' : '210px' }" @change="onDevChange">
        <el-option v-for="d in devices" :key="d.devid" :label="d.devid" :value="d.devid">
          <span>{{ d.devid }}</span>
          <el-tag v-if="!d.online" size="small" type="info" class="ml6">离线</el-tag>
        </el-option>
      </el-select>
      <el-button text :disabled="!msgs.length" @click="reset">新对话</el-button>
      <el-button v-if="compact" text @click="$router.push('/assistant')">全屏</el-button>
      <el-button text @click="openSetup">模型设置</el-button>
    </div>

    <div v-if="cfg && !cfg.ready" class="setup">
      <el-alert type="warning" :closable="false" show-icon>
        <template #title>还不能对话：{{ cfg.subject ? '这个账号还没配好模型' : '读不到账号信息' }}</template>
        <div class="setup-text">
          选一家服务商、贴一个 API Key 就能用，模型名有默认值。
          配置只属于你自己的账号，别人登录看到的是他自己那份。
        </div>
        <el-button size="small" type="primary" @click="openSetup">去配置</el-button>
      </el-alert>
    </div>

    <el-scrollbar ref="scroller" class="msgs" :height="chatHeight">
      <div v-if="!msgs.length" class="empty">
        <div class="empty-title">我可以帮你查这套系统里的实际情况</div>
        <div class="chips">
          <div v-for="q in suggest" :key="q" class="chip" @click="ask(q)">{{ q }}</div>
        </div>
        <div class="muted empty-tip">
          设备、会话、任务、租户、费用、权限组、证书、中继、仓库都能查；
          你要它改东西时，它会生成一张待确认作业卡（不会自己执行）。
        </div>
      </div>

      <div v-for="(m, i) in msgs" :key="i" class="msg" :class="m.role">
        <div class="bubble">
          <MarkdownView v-if="m.role === 'assistant'" :source="m.content" dense />
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
      <el-input v-model="input" type="textarea" :rows="compact ? 1 : 2" resize="none" maxlength="2000"
                :placeholder="devid ? ('问关于 ' + devid + ' 的问题，Enter 发送，Shift+Enter 换行') : '问点什么，Enter 发送，Shift+Enter 换行'"
                @keydown="onKey" />
      <el-button type="primary" :loading="busy" :disabled="!canSend" @click="send">发送</el-button>
    </div>
    <div v-if="!compact" class="muted foot">
      回答由模型生成，数据来自本平台的只读接口；涉及变更请走「作业」并走审批。
    </div>

    <!-- 模型设置：只填服务商 + 模型 + API Key，其它参数不给用户看 -->
    <el-dialog v-model="setupDlg" title="模型设置（只影响本账号）" width="560px">
      <el-form label-width="88px">
        <el-form-item label="服务商">
          <el-select v-model="form.provider" style="width: 100%" @change="onProvider">
            <el-option v-for="p in providers" :key="p.key" :label="p.label" :value="p.key">
              <span>{{ p.label }}</span>
              <span v-if="p.model" class="muted opt-model">{{ p.model }}</span>
            </el-option>
          </el-select>
        </el-form-item>
        <el-form-item label="模型">
          <el-input v-model="form.model" placeholder="选服务商后自动填默认，可改" maxlength="128" />
          <div v-if="providerHint" class="muted hint">{{ providerHint }}</div>
        </el-form-item>
        <el-form-item label="API Key">
          <el-input v-model="form.key" type="password" show-password maxlength="512"
                    :placeholder="keyPlaceholder" />
          <div class="muted hint">
            <template v-if="cfg && cfg.key_set">
              已保存 {{ cfg.key_hint }}<span v-if="cfg.key_updated_ms">（{{ time(cfg.key_updated_ms) }}）</span>，
              留空则保持不变。
            </template>
            <template v-else>密钥只写入 relay 本机文件，不进数据库、不回显。</template>
          </div>
        </el-form-item>
        <el-form-item label=" ">
          <el-button text type="primary" @click="showAdv = !showAdv">
            {{ showAdv ? '收起高级' : '高级（接口地址 / 自签证书）' }}
          </el-button>
        </el-form-item>
        <template v-if="showAdv">
          <el-form-item label="接口地址">
            <el-input v-model="form.endpoint" :placeholder="autoEndpoint" maxlength="300" />
            <div class="muted hint">留空使用服务商默认地址；自建网关填完整的 <code>…/v1</code>。</div>
          </el-form-item>
          <el-form-item label="证书">
            <el-checkbox v-model="form.insecure">跳过 TLS 证书校验（仅内网自签证书）</el-checkbox>
          </el-form-item>
        </template>
      </el-form>

      <div v-if="test" class="test" :class="{ bad: !test.ok }">
        <template v-if="test.ok">
          连接正常 · {{ test.latency_ms }}ms · 实际模型 {{ test.served_model || test.model }} ·
          回复「{{ test.reply }}」
        </template>
        <template v-else>{{ test.error }}</template>
      </div>

      <template #footer>
        <el-button v-if="cfg && cfg.key_set" text @click="clearKey">清除密钥</el-button>
        <span class="spacer" />
        <el-button :loading="testing" @click="testConn">测试连接</el-button>
        <el-button @click="setupDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveSetup">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, reactive, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { ElMessage } from 'element-plus'
import { Api } from '@/shared/api.js'
import { time } from '@/shared/utils.js'
import MarkdownView from '@/components/MarkdownView.vue'

const props = defineProps({
  // 仪表盘里嵌入时用 compact：固定高度、少一点留白、带“全屏”入口
  compact: { type: Boolean, default: false },
  // 从概览“AI 诊断”带过来的初始问题；只有全屏页绑定 query
  bindQuery: { type: Boolean, default: false },
})

const route = useRoute()

const SUGGEST_FULL = [
  '平台现在有什么异常？离线设备、失败任务、告警一起说',
  '最近谁改过权限或接入账号？',
  '哪些证书快过期了？',
  '这个月的流量和会话用量大概多少，多少钱？',
  '有没有失败的作业或自愈规则触发？',
  '最近 6 小时有没有设备的负载异常升高？',
]
const SUGGEST_COMPACT = [
  '平台现在有什么异常？',
  '哪些证书快过期了？',
  '最近谁改过权限？',
  '这个月大概多少钱？',
]

const suggest = computed(() => (props.compact ? SUGGEST_COMPACT : SUGGEST_FULL))
const chatHeight = computed(() => (props.compact ? '300px' : 'calc(100vh - 320px)'))

const cfg = ref(null)
const devices = ref([])
const devid = ref('')
const msgs = ref([])
const input = ref('')
const busy = ref(false)
const scroller = ref(null)
const canSend = computed(() => !busy.value && input.value.trim().length > 0 &&
  !!(cfg.value && cfg.value.ready))

// ---------------------------------------------------------------- 模型设置
// 只暴露三件事：服务商、模型名、API Key。地址与自签证书放在“高级”里，
// 因为绝大多数人用不上；max_tokens/temperature 之类固定在服务端，不问用户。
const setupDlg = ref(false)
const showAdv = ref(false)
const saving = ref(false)
const testing = ref(false)
const test = ref(null)
const form = reactive({ provider: 'deepseek', model: '', key: '', endpoint: '', insecure: false })

const providers = computed(() => (cfg.value && cfg.value.providers) || [])
const curProvider = computed(() => providers.value.find((p) => p.key === form.provider) || {})
const autoEndpoint = computed(() => curProvider.value.endpoint || '完整的 …/v1 地址')
const providerHint = computed(() => curProvider.value.hint || '')

function providerLabel(key) {
  const p = providers.value.find((x) => x.key === key)
  return p ? p.label : (key || '')
}

const keyPlaceholder = computed(() => {
  if (cfg.value && cfg.value.key_set) return '已配置，留空则保持不变'
  if (form.provider === 'ollama') return '本地模型随便填一个，例如 sk-local'
  return 'sk-… / 网关令牌'
})

function openSetup() {
  const c = cfg.value || {}
  form.provider = c.provider || 'deepseek'
  form.model = c.model || ''
  form.key = ''
  form.endpoint = c.endpoint || ''
  form.insecure = !!c.insecure
  // 有配置时高级区默认收起；自定义厂商或地址不是预置值时展开，否则用户看不到地址栏
  const def = providers.value.find((p) => p.key === form.provider)
  showAdv.value = form.provider === 'custom' || (!!def && !!def.endpoint && c.endpoint !== def.endpoint)
  test.value = null
  setupDlg.value = true
}

function onProvider() {
  const p = curProvider.value
  // 换服务商时把模型名带回默认值，避免留着上一家的模型名
  form.model = p.model || ''
  form.endpoint = ''
  if (p.key === 'custom') showAdv.value = true
  test.value = null
}

async function saveSetup() {
  if (!form.model.trim()) {
    ElMessage.warning('请填模型名')
    return
  }
  saving.value = true
  try {
    const body = { provider: form.provider, model: form.model.trim(), insecure: !!form.insecure }
    const ep = form.endpoint.trim()
    const def = curProvider.value
    if (ep && ep !== def.endpoint) body.endpoint = ep
    if (form.key.trim()) {
      await Api.put('/api/v1/ai/key', { key: form.key.trim() })
      form.key = ''
    }
    cfg.value = await Api.put('/api/v1/ai/config', body)
    test.value = null
    if (cfg.value.ready) {
      ElMessage.success('已保存，现在可以对话了')
      setupDlg.value = false
    } else {
      ElMessage.warning('已保存，但还缺 API Key')
    }
  } catch (e) {
    ElMessage.error(e.message || '保存失败')
  } finally {
    saving.value = false
  }
}

async function testConn() {
  testing.value = true
  test.value = null
  try {
    // 先用当前表单值落库，测试才有意义（否则测的是旧配置）
    if (form.key.trim() || form.model.trim()) {
      const body = { provider: form.provider, model: form.model.trim(), insecure: !!form.insecure }
      const ep = form.endpoint.trim()
      if (ep && ep !== curProvider.value.endpoint) body.endpoint = ep
      if (form.key.trim()) {
        await Api.put('/api/v1/ai/key', { key: form.key.trim() })
        form.key = ''
      }
      cfg.value = await Api.put('/api/v1/ai/config', body)
    }
    test.value = await Api.post('/api/v1/ai/test', {})
  } catch (e) {
    test.value = { ok: false, error: e.message || '测试失败' }
  } finally {
    testing.value = false
  }
}

async function clearKey() {
  try {
    cfg.value = await Api.del('/api/v1/ai/key')
    test.value = null
    ElMessage.success('密钥已清除')
  } catch (e) {
    ElMessage.error(e.message || '清除失败')
  }
}

function argsText(a) {
  if (!a || typeof a !== 'object') return ''
  const s = Object.entries(a).map(([k, v]) => k + '=' + v).join(' ')
  return s.length > 60 ? s.slice(0, 60) + '…' : s
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
  if (!cfg.value || !cfg.value.ready) {
    ElMessage.warning('先点「模型设置」选一个服务商并填 API Key')
    openSetup()
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

// 概览页的“AI 诊断”带 ?q= 到全屏页
function applyQuery() {
  if (!props.bindQuery) return
  const q = route.query.q
  if (q) input.value = String(q)
}
watch(() => route.query.q, applyQuery)

defineExpose({ ask })   // 页面上的“AI 诊断”直接把问题发进来，不用跳转

onMounted(() => {
  loadCfg()
  loadDevices()
  applyQuery()
})
</script>

<style scoped>
.ai-chat { display: flex; flex-direction: column; }
.mini-title { font-size: 14px; font-weight: 650; }
.setup { margin: 8px 0 4px; }
.setup-text { margin: 4px 0 8px; font-size: 12.5px; }
.opt-model { float: right; font-size: 12px; }
.hint { font-size: 12px; line-height: 1.6; }
.hint code { background: var(--el-fill-color); padding: 1px 4px; border-radius: 3px; }
.test { margin-top: 4px; font-size: 12.5px; color: var(--el-color-success); word-break: break-all; }
.test.bad { color: var(--el-color-danger); }
.msgs { padding: 4px 2px 8px; }
.empty { padding: 18px 8px; }
.empty-title { font-size: 14.5px; margin-bottom: 10px; }
.empty-tip { margin-top: 10px; font-size: 12.5px; }
.chips { display: flex; flex-wrap: wrap; gap: 8px; }
.chip {
  border: 1px solid var(--el-border-color);
  border-radius: 14px;
  padding: 6px 11px;
  font-size: 12.5px;
  cursor: pointer;
}
.chip:hover { border-color: var(--el-color-primary); color: var(--el-color-primary); }
.msg { display: flex; margin: 10px 0; }
.msg.user { justify-content: flex-end; }
.bubble {
  max-width: 84%;
  padding: 9px 12px;
  border-radius: 10px;
  background: var(--el-fill-color-light);
  font-size: 13.5px;
  line-height: 1.65;
  word-break: break-word;
}
.msg.user .bubble { background: var(--el-color-primary-light-9); }
.plain { white-space: pre-wrap; }
.bubble :deep(.md-code) { margin: 8px 0; }
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
