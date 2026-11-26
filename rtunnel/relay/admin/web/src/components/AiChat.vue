<template>
  <div ref="root" class="ai-chat">
    <div class="panel-head">
      <h2>AI 助手</h2>
      <el-tag v-if="cfg" size="small" :type="cfg.ready ? 'success' : 'warning'">
        {{ cfg.ready ? (providerLabel(cfg.provider) + ' · ' + cfg.model) : '未配置' }}
      </el-tag>
      <span class="spacer" />
      <el-select v-model="devid" clearable filterable placeholder="设备上下文（可选）"
                 :style="{ width: '210px' }" @change="onDevChange">
        <el-option v-for="d in devices" :key="d.devid" :label="d.devid" :value="d.devid">
          <span>{{ d.devid }}</span>
          <el-tag v-if="!d.online" size="small" type="info" class="ml6">离线</el-tag>
        </el-option>
      </el-select>
      <el-button text @click="openHistory">历史{{ sessions.length ? '（' + sessions.length + '）' : '' }}</el-button>
      <el-button text :disabled="!msgs.length" @click="reset">新对话</el-button>
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

    <div class="msgs">
      <div v-if="!msgs.length" class="empty">
        <div class="empty-mark">◈</div>
        <div class="empty-title">我是 rtunnel 助手</div>
        <div class="muted empty-lead">
          设备、会话、任务、证书、费用、权限都能查。要改东西时，我会先给你一张待确认作业卡。
        </div>
        <div class="chips">
          <div v-for="q in suggest" :key="q" class="chip" @click="ask(q)">{{ q }}</div>
        </div>
        <div class="muted empty-tip">
          也可以直接说“重启一下 nginx”“清理 /tmp 旧包”，我会生成作业卡等你要确认。
        </div>
      </div>

      <div v-for="(m, i) in msgs" :key="m._k || i" class="msg" :class="m.role">
        <div class="bubble">
          <!-- 出错时可能没有正文：把错误显示在气泡里，别让 MarkdownView 只剩“这里还没有内容” -->
          <MarkdownView v-if="m.role === 'assistant' && m.content" :source="m.content" dense />
          <div v-else-if="m.role === 'assistant'" class="err">
            {{ m.error || '这次没有回答内容，可以再问一次。' }}
          </div>
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
          <div v-if="m.error && m.content" class="err">{{ m.error }}</div>
          <div v-if="m.meta" class="muted meta">{{ m.meta }}</div>
        </div>
      </div>

      <div v-if="busy" class="msg assistant">
        <div class="bubble thinking">
          <span class="dot" /><span class="dot" /><span class="dot" />
          <span class="muted">正在查数据…</span>
        </div>
      </div>
    </div>

    <div class="composer">
      <div class="composer-inner">
        <el-input v-model="input" type="textarea" :autosize="{ minRows: 1, maxRows: 6 }" resize="none"
                  maxlength="2000" class="composer-input"
                  :placeholder="devid ? ('问关于 ' + devid + ' 的问题…') : '问点什么，设备、任务、证书、费用、权限都能查…'"
                  @keydown="onKey" />
        <el-button class="send-btn" type="primary" :loading="busy" :disabled="!canSend" @click="send">
          发送
        </el-button>
      </div>
      <div class="muted compose-hint">
        Enter 发送 · Shift + Enter 换行
        <span class="spacer" />
        <span class="compose-note">回答由模型生成，数据来自本平台；变更请走「作业」并走审批</span>
      </div>
    </div>

    <!-- 头部和输入框都随页面滚走，往上翻之后用这个直接回到底端 -->
    <transition name="fade">
      <el-button v-if="!atBottom" class="to-bottom" @click="scrollBottom(true)">
        <span class="to-bottom-arrow">↓</span> 回到底端
      </el-button>
    </transition>

    <!-- 历史会话：最近 10 个在前，可继续往前翻；记录永久保留，只能主动删除 -->
    <el-drawer v-model="histOpen" size="420px" title="历史会话">
      <div v-loading="histLoading" class="hist">
        <div v-if="!sessions.length && !histLoading" class="muted hist-empty">
          还没有历史会话。提问之后会自动保存，关掉页面也不会丢。
        </div>
        <div v-for="s in sessions" :key="s.id" class="hist-item" :class="{ on: s.id === sessionId }">
          <div class="hist-main" @click="openSession(s)">
            <div class="hist-title">{{ s.title || '未命名会话' }}</div>
            <div class="muted hist-sub">
              {{ ago(s.updated_ms) }}前 · {{ s.msg_count }} 条
              <span v-if="s.id === sessionId" class="hist-cur">当前</span>
            </div>
          </div>
          <div class="hist-ops">
            <el-button text size="small" @click.stop="renameSession(s)">改名</el-button>
            <el-button text size="small" @click.stop="removeSession(s)">删除</el-button>
          </div>
        </div>
        <el-button v-if="histHasMore" text class="hist-more" :loading="histLoading"
                   @click="loadSessions(true)">
          加载更早的会话
        </el-button>
      </div>
    </el-drawer>

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
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { ago, time } from '@/shared/utils.js'
import MarkdownView from '@/components/MarkdownView.vue'

const props = defineProps({
  // 从概览“AI 诊断”带过来的初始问题：进页面后直接发问
  bindQuery: { type: Boolean, default: false },
})

const route = useRoute()
const router = useRouter()

const SUGGEST_FULL = [
  '平台现在有什么异常？离线设备、失败任务、告警一起说',
  '最近谁改过权限或接入账号？',
  '哪些证书快过期了？',
  '这个月的流量和会话用量大概多少，多少钱？',
  '有没有失败的作业或自愈规则触发？',
  '最近 6 小时有没有设备的负载异常升高？',
]
const suggest = SUGGEST_FULL

const cfg = ref(null)
const devices = ref([])
const devid = ref('')
// 当前会话：0 = 还没落库（第一句话发出时服务端建，并把 id 带回来）
const sessionId = ref(0)
const sessions = ref([])
const histOpen = ref(false)
const histLoading = ref(false)
const histHasMore = ref(false)
const histCursor = ref({ before: 0, before_id: 0 })
const msgs = ref([])
const input = ref('')
const busy = ref(false)
const root = ref(null)
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

// 主区 .page 是滚动容器：头部/输入框都不吸附，往上翻就跟着走；
// 只有“回到底端”按钮浮在右下角，离开底部时出现。
const atBottom = ref(true)
let pageEl = null
function onPageScroll() {
  if (!pageEl) return
  atBottom.value = pageEl.scrollHeight - pageEl.scrollTop - pageEl.clientHeight < 80
}
function bindPageScroll() {
  const el = root.value && root.value.closest('.page')
  if (!el || el === pageEl) {
    if (el) onPageScroll()
    return
  }
  if (pageEl) pageEl.removeEventListener('scroll', onPageScroll)
  pageEl = el
  pageEl.addEventListener('scroll', onPageScroll, { passive: true })
  onPageScroll()
}

// 页面级滚动：消息是追加到 iflow 里的节点，滚动的是主区 .page，
// 不是对话内部的小窗口。用户往上翻看历史时不要拽回来。
async function scrollBottom(force = false) {
  await nextTick()
  const el = root.value
  if (!el) return
  const box = el.closest('.page')
  if (!box) return
  const near = box.scrollHeight - box.scrollTop - box.clientHeight < 220
  if (force || near) box.scrollTo({ top: box.scrollHeight, behavior: 'smooth' })
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

// ---------------------------------------------------------------- 历史会话
// 记录永久保留：这里不做过期清理，删除只由用户点。
// 列表默认最近 10 个，翻页用服务端给的 (before, before_id) 复合游标。
async function loadSessions(more = false) {
  histLoading.value = true
  try {
    const q = more
      ? '?limit=10&before=' + histCursor.value.before + '&before_id=' + histCursor.value.before_id
      : '?limit=10'
    const r = await Api.get('/api/v1/ai/sessions' + q)
    const items = r.items || []
    sessions.value = more ? sessions.value.concat(items) : items
    histHasMore.value = !!r.has_more
    histCursor.value = { before: r.next_before_ms || 0, before_id: r.next_before_id || 0 }
  } catch (e) {
    if (!more) sessions.value = []
  } finally {
    histLoading.value = false
  }
}

function openHistory() {
  histOpen.value = true
  loadSessions(false)
}

async function openSession(s) {
  if (busy.value) {
    ElMessage.warning('等这条回答结束再切换')
    return
  }
  try {
    const d = await Api.get('/api/v1/ai/sessions/' + s.id)
    sessionId.value = d.id
    msgs.value = (d.messages || []).map((m) => {
      const out = { role: m.role, content: m.content || '' }
      if (m.actions && m.actions.length) out.actions = m.actions
      if (m.tools && m.tools.length) out.tools = m.tools
      if (m.meta) out.meta = m.meta
      if (m.error) out.error = m.error
      return out
    })
    histOpen.value = false
    scrollBottom(true)
  } catch (e) {
    ElMessage.error(e.message || '读取会话失败')
  }
}

async function renameSession(s) {
  try {
    const { value } = await ElMessageBox.prompt('会话标题', '改名', {
      inputValue: s.title || '',
      inputPlaceholder: '例如：mp135 掉线排查',
      inputValidator: (v) => (v && v.trim() ? true : '不能为空'),
    })
    const t = (value || '').trim()
    if (!t) return
    const r = await Api.put('/api/v1/ai/sessions/' + s.id, { title: t })
    s.title = r.title || t
    ElMessage.success('已改名')
  } catch (e) {
    if (e !== 'cancel' && e !== 'close') ElMessage.error(e.message || '改名失败')
  }
}

async function removeSession(s) {
  try {
    await ElMessageBox.confirm('删除后这条会话就找不回来了（其它会话不受影响）', '删除会话', {
      type: 'warning',
      confirmButtonText: '删除',
    })
  } catch (e) {
    return
  }
  try {
    await Api.del('/api/v1/ai/sessions/' + s.id)
    sessions.value = sessions.value.filter((x) => x.id !== s.id)
    if (sessionId.value === s.id) reset()
    ElMessage.success('已删除')
  } catch (e) {
    ElMessage.error(e.message || '删除失败')
  }
}

function reset() {
  msgs.value = []
  input.value = ''
  sessionId.value = 0
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
  scrollBottom(true)
  // 只发最近 20 条，避免上下文过长
  const history = msgs.value.slice(-20).map((m) => ({ role: m.role, content: m.content }))
  try {
    const r = await Api.post('/api/v1/ai/chat', {
      messages: history,
      devid: devid.value || undefined,
      session_id: sessionId.value || undefined,
    })
    if (r.session_id) sessionId.value = r.session_id
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
    scrollBottom(true)
    touchSessionLocal()
  }
}

// 本地同步：会话已经在服务端落库，这里只更新侧栏显示，避免每次发完都多一次请求
function touchSessionLocal() {
  if (!sessionId.value) return
  const i = sessions.value.findIndex((x) => x.id === sessionId.value)
  if (i < 0) {
    loadSessions(false)
    return
  }
  const s = sessions.value[i]
  s.msg_count = msgs.value.length
  s.updated_ms = Date.now()
  if (!s.title) {
    const first = msgs.value.find((m) => m.role === 'user')
    if (first) s.title = first.content.slice(0, 30)
  }
  sessions.value.splice(i, 1)
  sessions.value.unshift(s)
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
  if (!q) return
  input.value = String(q)
  // 用掉 query：刷新不会重复发问
  const rest = { ...route.query }
  delete rest.q
  router.replace({ path: route.path, query: rest })
  nextTick(() => ask(input.value))
}
watch(() => route.query.q, applyQuery)

defineExpose({ ask })   // 页面上的“AI 诊断”直接把问题发进来，不用跳转

onMounted(() => {
  loadCfg()
  loadDevices()
  loadSessions(false)   // 只为按钮上的数量，失败静默
  applyQuery()
  nextTick(bindPageScroll)
})

onBeforeUnmount(() => {
  if (pageEl) pageEl.removeEventListener('scroll', onPageScroll)
  pageEl = null
})
</script>

<style scoped>
/* 首页即对话页：不套仪表盘卡片，整块居中，头部/消息/输入框同一文档流 */
.ai-chat {
  display: flex;
  flex-direction: column;
  min-width: 0;
  width: 100%;
  max-width: 920px;
  margin: 0 auto;
  min-height: calc(100vh - 64px - 2 * var(--page-pad));
}
.ai-chat .panel-head h2 { white-space: nowrap; }
.ai-chat .panel-head {
  margin: 0 0 4px;
  padding: 4px 0 14px;
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.ai-chat .msgs {
  flex: 1;
  display: flex;
  flex-direction: column;
  padding: 18px 0 8px;
}
/* 消息列限宽居中；空状态在可用高度里居中 */
.ai-chat .msg { width: 100%; max-width: 880px; margin-left: auto; margin-right: auto; }
.ai-chat .empty {
  width: 100%;
  max-width: 760px;
  margin: auto;
  padding: 24px 8px 48px;
  text-align: center;
}
.empty-mark { font-size: 30px; color: var(--el-color-primary); line-height: 1; }
.empty-title { font-size: 22px; font-weight: 650; margin: 14px 0 8px; letter-spacing: 0.01em; }
.empty-lead { font-size: 13.5px; line-height: 1.8; }
.ai-chat .empty .chips { justify-content: center; margin: 22px 0 12px; }
.empty-tip { font-size: 12.5px; }
.ai-chat .composer {
  width: 100%;
  max-width: 820px;
  margin: auto auto 0;
  padding: 10px 0 12px;
}
@media (max-width: 599px) {
  .ai-chat { min-height: calc(100vh - 56px - 2 * var(--page-pad)); }
  /* 手机上头部两行：标题+按钮一行，设备上下文单独一行 */
  .ai-chat .panel-head { flex-wrap: wrap; row-gap: 8px; padding: 2px 0 10px; }
  .ai-chat .panel-head .el-tag, .ai-chat .panel-head .spacer { display: none; }
  .ai-chat .panel-head .el-select { order: 10; width: 100% !important; }
  .compose-note { display: none; }
  .ai-chat .msgs { padding: 12px 0 6px; }
  .ai-chat .composer { padding: 8px 0 10px; }
  .empty-title { font-size: 20px; }
}
.to-bottom {
  position: fixed;
  right: 32px;
  bottom: 32px;
  z-index: 30;
  height: 34px;
  padding: 0 14px;
  border-radius: 17px;
  background: var(--el-bg-color);
  box-shadow: 0 6px 20px rgba(0, 0, 0, 0.16);
}
.to-bottom-arrow { margin-right: 4px; font-weight: 700; }
.fade-enter-active, .fade-leave-active { transition: opacity 0.18s ease, transform 0.18s ease; }
.fade-enter-from, .fade-leave-to { opacity: 0; transform: translateY(6px); }
@media (max-width: 599px) {
  .to-bottom { right: 14px; bottom: 18px; }
}
.setup { margin: 8px 0 4px; }
.setup-text { margin: 4px 0 8px; font-size: 12.5px; }
.opt-model { float: right; font-size: 12px; }
.hint { font-size: 12px; line-height: 1.6; }
.hint code { background: var(--el-fill-color); padding: 1px 4px; border-radius: 3px; }
.test { margin-top: 4px; font-size: 12.5px; color: var(--el-color-success); word-break: break-all; }
.test.bad { color: var(--el-color-danger); }
.hist { display: flex; flex-direction: column; gap: 6px; min-height: 60px; }
.hist-empty { padding: 12px 2px; font-size: 12.5px; line-height: 1.7; }
.hist-item {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 7px 9px;
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 7px;
  transition: border-color 0.15s ease, background 0.15s ease;
}
.hist-item:hover { border-color: var(--el-color-primary-light-5); background: var(--el-fill-color-lighter); }
.hist-item.on { border-color: var(--el-color-primary); background: var(--el-color-primary-light-9); }
.hist-main { flex: 1; min-width: 0; cursor: pointer; }
.hist-title {
  font-size: 13px;
  font-weight: 550;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.hist-sub { font-size: 11.5px; margin-top: 2px; }
.hist-cur { color: var(--el-color-primary); margin-left: 4px; }
.hist-ops { display: flex; flex: none; opacity: 0; transition: opacity 0.15s ease; }
.hist-item:hover .hist-ops { opacity: 1; }
.hist-more { margin-top: 4px; }
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
.msg { display: flex; margin: 14px 0; }
.msg:first-child { margin-top: 4px; }
.msg.user { justify-content: flex-end; }
.bubble {
  max-width: 88%;
  padding: 11px 14px;
  border-radius: 12px;
  background: var(--el-fill-color-light);
  font-size: 13.5px;
  line-height: 1.7;
  word-break: break-word;
}
.msg.assistant .bubble { border: 1px solid var(--el-border-color-lighter); }
.msg.user .bubble {
  max-width: 76%;
  background: var(--el-color-primary-light-8);
  border: 1px solid var(--el-color-primary-light-7);
}
.ai-chat .bubble { max-width: min(860px, 92%); }
.ai-chat .msg.user .bubble { max-width: min(680px, 76%); }
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
.composer { margin-top: 10px; }
.composer-inner {
  display: flex;
  align-items: flex-end;
  gap: 8px;
  padding: 5px 6px 5px 10px;
  border: 1px solid var(--el-border-color);
  border-radius: 12px;
  background: var(--el-bg-color);
  transition: border-color 0.15s ease, box-shadow 0.15s ease;
}
.composer-inner:focus-within {
  border-color: var(--el-color-primary);
  box-shadow: 0 0 0 3px var(--el-color-primary-light-9);
}
.composer-input { flex: 1; min-width: 0; }
.composer-input :deep(.el-textarea__inner) {
  box-shadow: none !important;
  background: transparent;
  padding: 4px 0;
  font-size: 13.5px;
}
.send-btn { flex: none; }
.compose-hint {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-top: 6px;
  font-size: 11.5px;
}
.ml6 { margin-left: 6px; }
</style>
