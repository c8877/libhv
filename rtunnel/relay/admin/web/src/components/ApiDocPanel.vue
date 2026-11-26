<template>
  <div class="api-docs" :class="{ compact }">
    <div v-if="token" class="token-head">
      <div class="token-title">
        <b>{{ token.name || '未命名 Token' }}</b>
        <span class="mono muted">{{ token.token_prefix }}</span>
      </div>
      <div class="token-meta">
        <el-tag size="small" type="info">{{ token.username }}</el-tag>
        <el-tag v-if="token.group_count > 0" size="small" type="warning">
          {{ token.group_count }} 个权限组
        </el-tag>
        <el-tag v-else size="small" type="info">未绑定权限组 = 无权限</el-tag>
      </div>
    </div>

    <el-tabs v-model="tab" class="tabs">
      <!-- ---------------------------------------------------------- 快速开始 -->
      <el-tab-pane label="快速开始" name="start">
        <div class="sec-title">① 设置环境变量</div>
        <div class="code-wrap">
          <pre class="code">{{ envSnippet }}</pre>
          <el-button size="small" text class="copy" @click="copy(envSnippet)">复制</el-button>
        </div>
        <div class="muted">
          管理台用自签证书时，curl 会报 <code>SSL certificate problem: self-signed certificate</code>：
          给 curl 加 <code>-k</code>，或 <code>export CURL_CA_BUNDLE=/path/admin.crt</code>（自签证书默认在
          relay 启动目录的 <code>admin-tls/admin.crt</code>，配了 <code>--admin-cert-dir</code> 则在
          <code>&lt;cert-dir&gt;/admin-tls/</code> 下）。
        </div>

        <div class="sec-title">② 验证身份</div>
        <div class="code-wrap">
          <pre class="code">{{ verifySnippet }}</pre>
          <el-button size="small" text class="copy" @click="copy(verifySnippet)">复制</el-button>
        </div>
        <div class="muted">返回 200 表示 Token 可用；401 检查是否停用/过期，403 检查资源权限与设备范围（权限只来自权限组）。</div>

        <div class="sec-title">③ 常见用法</div>
        <div class="recipes">
          <div v-for="r in recipes" :key="r.name" class="recipe">
            <div class="recipe-head">
              <span>{{ r.name }}</span>
              <el-button size="small" text @click="copy(r.curl)">复制</el-button>
            </div>
            <pre class="code">{{ r.curl }}</pre>
          </div>
        </div>

        <div class="sec-title">④ 给 AI / 自动化用（OpenAPI + MCP）</div>
        <div class="code-wrap">
          <pre class="code">{{ aiSnippet }}</pre>
          <el-button size="small" text class="copy" @click="copy(aiSnippet)">复制</el-button>
        </div>
        <div class="muted">
          <code>/api/v1/openapi.json</code> 由接口文档生成（与路由表对表），接口变了它自动变。
          <code>rt-mcp.py</code> 默认只把 GET 暴露成工具；写操作要显式 <code>--allow-write</code>，
          真执行仍需令牌的资源权限，并可通过 <code>operations</code> 走审批。
        </div>

        <div class="sec-title">⑤ WebSocket（文件/服务/终端）</div>
        <div class="code-wrap">
          <pre class="code">{{ wsSnippet }}</pre>
          <el-button size="small" text class="copy" @click="copy(wsSnippet)">复制</el-button>
        </div>
        <div class="muted">
          浏览器用登录会话 Cookie 直接连接；脚本在握手时带
          <code>Authorization: Bearer $ATK</code>，不要把令牌放进查询串。
        </div>

        <div class="sec-title">错误码</div>
        <div class="err-grid">
          <div v-for="e in ERRORS" :key="e[0]" class="err-row">
            <span class="err-code mono">{{ e[0] }}</span>
            <span class="muted">{{ e[1] }}</span>
          </div>
        </div>
      </el-tab-pane>

      <!-- ---------------------------------------------------------- 接口清单 -->
      <el-tab-pane :label="`接口清单 (${visible.length})`" name="api">
        <div class="toolbar">
          <el-input v-model="q" placeholder="搜索路径 / 说明 / 分组" clearable class="search">
            <template #prefix><el-icon><Search /></el-icon></template>
          </el-input>
          <el-checkbox v-model="onlyRead">只看只读接口</el-checkbox>
          <span class="muted hidden-tip" v-if="readOnlyCount">只读 {{ readOnlyCount }} 条</span>
        </div>

        <div v-if="!visible.length" class="muted empty">没有匹配的接口</div>

        <div v-for="g in groupList" :key="g.key" class="group">
          <div class="group-head" @click="toggleGroup(g.key)">
            <el-icon class="chev" :class="{ open: openGroups.has(g.key) }"><ArrowRight /></el-icon>
            <b>{{ g.label }}</b>
            <span class="muted">{{ g.items.length }}</span>
          </div>
          <div v-show="openGroups.has(g.key)" class="group-body">
            <div v-for="(d, i) in g.items" :key="g.key + i" class="ep">
              <div class="ep-head" @click="toggleEp(d)">
                <span class="method" :class="d.method.toLowerCase()">{{ d.method }}</span>
                <span class="path mono">{{ d.path }}</span>
                <span class="spacer" />
                <el-tag size="small" :type="d.method === 'GET' ? 'info' : 'warning'" class="role-tag">
                  {{ d.method === 'GET' ? '只读' : '写' }}
                </el-tag>
                <el-icon class="chev sm" :class="{ open: isOpen(d) }"><ArrowRight /></el-icon>
              </div>
              <div class="ep-summary">{{ d.summary }}</div>
              <div v-show="isOpen(d)" class="ep-body">
                <div v-if="d.note" class="note">{{ d.note }}</div>
                <!-- ui-allow: 接口参数是固定文档展示表，没有排序/分页/选择，不适合 el-table -->
                <table v-if="d.params && d.params.length" class="params">
                  <tr v-for="p in d.params" :key="p.name">
                    <td class="pname mono">{{ p.name }}</td>
                    <td class="pin muted">{{ p.in }}</td>
                    <td class="muted">{{ p.desc }}</td>
                  </tr>
                </table>
                <div class="code-wrap">
                  <pre class="code">{{ d.curl }}</pre>
                  <el-button size="small" text class="copy" @click="copy(d.curl)">复制 curl</el-button>
                </div>
                <template v-if="d.resp">
                  <div class="resp-label muted">响应示例</div>
                  <pre class="code resp">{{ d.resp }}</pre>
                </template>
              </div>
            </div>
          </div>
        </div>
      </el-tab-pane>

      <!-- ---------------------------------------------------------- 权限范围 -->
      <el-tab-pane label="权限范围" name="scope">
        <div class="sec-title">权限只来自权限组</div>
        <div class="muted scope-text">
          每个接口要求一个资源键（<code>device.view</code>、<code>device.exec</code>、
          <code>fs.read</code>、<code>repo.write</code>…）。账号/Token 能调什么，取决于它所在的权限组
          勾了哪些键；<b>账号角色本身不授予任何权限</b>，只用于账号管理页的层级（不能操作比自己高的角色）。
          未分组的账号没有任何资源权限。
        </div>
        <div class="muted scope-text">
          权限组还要给「设备范围」：没勾「全部设备」时，设备类接口只能看到并操作组内设备。
          这也是接入令牌（<code>rtk-</code>）能被收窄到单台设备的方式。
        </div>

        <div class="sec-title">设备范围</div>
        <div class="muted scope-text">
          <template v-if="token && token.group_count > 0">
            该 Token 直接绑定了 {{ token.group_count }} 个权限组（{{ token.groups || '—' }}）；
            <b>有令牌级分组时只认这些组</b>，不再继承归属账号的其它分组。
          </template>
          <template v-else>
            该 Token 未单独绑定分组，继承归属账号所在分组的并集。
          </template>
          无论哪种，设备类接口都只返回授权范围内的设备；设备不在范围内时返回 403。
        </div>

        <div class="sec-title">API Token 不能做的事</div>
        <ul class="limits">
          <li v-for="l in ATK_UNAVAILABLE" :key="l">{{ l }}</li>
          <li>接入令牌（<code>rtk-</code>）不继承运营商侧资源（权限组/字典/中继/证书/设置/atk 管理/审批），
            即使被误授也不生效；只看得到本租户设备。</li>
        </ul>
      </el-tab-pane>
    </el-tabs>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { ArrowRight, Search } from '@element-plus/icons-vue'
import { copyText } from '@/shared/utils.js'
import { ATK_UNAVAILABLE, DOCS, ERRORS, GROUPS } from '@/shared/api-docs.js'

const props = defineProps({
  token: { type: Object, default: null },
  compact: { type: Boolean, default: false },
})

const tab = ref('start')
const q = ref('')
const onlyRead = ref(false)
const openGroups = ref(new Set(['device']))
const expanded = ref(new Set())

const readOnlyCount = computed(() => DOCS.filter((d) => d.method === 'GET').length)

// 不再按角色过滤：能否调用由权限组决定，文档对所有账号完整展示
const baseDocs = computed(() =>
  onlyRead.value ? DOCS.filter((d) => d.method === 'GET') : DOCS.slice())

const visible = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return baseDocs.value
  return baseDocs.value.filter((d) =>
    d.path.toLowerCase().includes(k) ||
    d.summary.toLowerCase().includes(k) ||
    d.method.toLowerCase().includes(k) ||
    (GROUPS.find((g) => g.key === d.group)?.label || '').toLowerCase().includes(k))
})

const groupList = computed(() =>
  GROUPS.map((g) => ({ key: g.key, label: g.label, items: visible.value.filter((d) => d.group === g.key) }))
    .filter((g) => g.items.length))

const envSnippet = computed(() =>
  `export RT=${location.origin}\nexport ATK=atk-xxxxxxxx   # 替换为创建时保存的明文`)

const verifySnippet = `# 自签证书加 -k\ncurl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/me"`

const wsSnippet = computed(() => {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  return `# 浏览器：登录后直接连，不带令牌\n` +
    `${proto}://${location.host}/ws/fs/<devid>\n` +
    `# 脚本：握手带 Authorization 头（令牌不要放查询串）\n` +
    `websocket.create_connection("${proto}://${location.host}/ws/fs/<devid>",\n` +
    `    header=["Authorization: Bearer " + os.environ["ATK"]])\n` +
    `# 另：/ws/svc/<devid>、/ws/tty/<devid>/<alias>、/ws/proxy/<devid>/<port>`
})

const recipes = [
  { name: '列设备', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents"` },
  { name: '看资源占用', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/sysinfo?mode=inc"` },
  { name: '上传固件', curl: `curl -k -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @fw.bin \\\n  "$RT/api/v1/files/fw.bin"` },
  { name: '分发到多台', curl: `curl -k -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"file":"fw.bin","devids":["mp135","mp136"],"dir":"firmware"}' \\\n  "$RT/api/v1/distributions"` },
  { name: '查审计', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/audit"` },
]

const aiSnippet = computed(() =>
  `# 1) 生成物：OpenAPI（由本页接口文档生成，与路由表对表）\n` +
  `curl -k -sS ${location.origin}/api/v1/openapi.json | head\n\n` +
  `# 2) MCP server（只读；写操作加 --allow-write）\n` +
  `python3 rt-mcp.py --rt ${location.origin} --atk-file /etc/rtunnel/atk --insecure\n\n` +
  `# 客户端配置（Claude Desktop 等）\n` +
  `{"mcpServers":{"rtunnel":{"command":"python3","args":["/opt/rtunnel/deploy/rt-mcp.py",\n` +
  `  "--rt","${location.origin}","--atk-file","/etc/rtunnel/atk","--insecure"]}}}`)

function isOpen(d) {
  return expanded.value.has(d.path + d.method)
}
function toggleEp(d) {
  const k = d.path + d.method
  const s = new Set(expanded.value)
  s.has(k) ? s.delete(k) : s.add(k)
  expanded.value = s
}
function toggleGroup(k) {
  const s = new Set(openGroups.value)
  s.has(k) ? s.delete(k) : s.add(k)
  openGroups.value = s
}
function copy(text) {
  copyText(text, '已复制')
}
</script>

<style scoped>
.api-docs { font-size: 13px; }
.api-docs.compact { font-size: 12.5px; }

.token-head {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
  padding: 0 2px 10px;
  border-bottom: 1px solid var(--el-border-color-lighter);
  margin-bottom: 4px;
}
.token-title { display: flex; align-items: baseline; gap: 8px; }
.token-meta { display: flex; gap: 6px; flex-wrap: wrap; margin-left: auto; }

.tabs { margin-top: 2px; }

.sec-title {
  margin: 16px 0 8px;
  font-weight: 650;
  font-size: 13px;
}
.sec-title:first-child { margin-top: 4px; }

.code-wrap { position: relative; }
.code {
  margin: 0 0 8px;
  padding: 10px 12px;
  border-radius: 8px;
  background: var(--el-fill-color-light);
  border: 1px solid var(--el-border-color-lighter);
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 11.5px;
  line-height: 1.65;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  color: var(--el-text-color-regular);
}
.code.resp { opacity: 0.9; }
.copy { position: absolute; top: 2px; right: 4px; opacity: 0.7; }
.copy:hover { opacity: 1; }

.recipes { display: flex; flex-direction: column; gap: 6px; }
.recipe { border: 1px solid var(--el-border-color-lighter); border-radius: 8px; padding: 6px 8px; }
.recipe-head {
  display: flex; align-items: center; justify-content: space-between;
  font-weight: 550; margin-bottom: 4px;
}
.recipe .code { margin-bottom: 0; background: transparent; border: 0; padding: 2px 0; }

.err-grid { display: flex; flex-direction: column; gap: 4px; }
.err-row { display: flex; gap: 10px; align-items: baseline; }
.err-code {
  min-width: 34px; color: var(--el-color-danger); font-weight: 650;
}

.toolbar {
  display: flex; align-items: center; gap: 12px; margin: 6px 0 12px; flex-wrap: wrap;
}
.search { width: 280px; }
.hidden-tip { font-size: 12px; }

.group { border-top: 1px solid var(--el-border-color-lighter); }
.group:first-of-type { border-top: 0; }
.group-head {
  display: flex; align-items: center; gap: 8px;
  padding: 10px 2px; cursor: pointer; user-select: none;
}
.group-body { padding: 0 0 8px 6px; }

.ep { border-radius: 8px; padding: 6px 8px; }
.ep:hover { background: var(--el-fill-color-light); }
.ep-head { display: flex; align-items: center; gap: 8px; cursor: pointer; }
.ep-head .spacer { flex: 1; }
.method {
  min-width: 48px; text-align: center; font-size: 10.5px; font-weight: 700;
  padding: 2px 6px; border-radius: 5px; background: var(--el-fill-color-dark);
  color: var(--el-text-color-regular);
}
.method.get { color: #0b7285; background: #e3fafc; }
.method.post { color: #2b8a3e; background: #ebfbee; }
.method.put { color: #9c6b00; background: #fff8e1; }
.method.delete { color: #c92a2a; background: #fff0f0; }
html.dark .method.get { background: #0b3b44; color: #99e9f2; }
html.dark .method.post { background: #12351f; color: #b2f2bb; }
html.dark .method.put { background: #3a2f0b; color: #ffe08a; }
html.dark .method.delete { background: #3b1414; color: #ffc9c9; }

.path { font-size: 12px; }
.role-tag { flex: none; }
.chev { transition: transform 0.15s ease; }
.chev.open { transform: rotate(90deg); }
.chev.sm { font-size: 12px; color: var(--el-text-color-secondary); }
.ep-summary { color: var(--el-text-color-secondary); padding: 2px 0 0 56px; }
.ep-body { padding: 8px 0 4px 56px; }
.note {
  margin-bottom: 8px; padding: 6px 8px; border-radius: 6px;
  background: var(--el-fill-color-lighter); color: var(--el-text-color-secondary);
  font-size: 12px; line-height: 1.6;
}
.params { border-collapse: collapse; margin-bottom: 8px; width: 100%; }
.params td { padding: 2px 8px 2px 0; vertical-align: top; }
.pname { color: var(--el-color-primary); }
.pin { width: 42px; }
.resp-label { margin: 6px 0 4px; font-size: 11.5px; }

.empty { padding: 24px 0; text-align: center; }

.role-table { display: flex; flex-direction: column; gap: 6px; }
.role-row {
  border: 1px solid var(--el-border-color-lighter); border-radius: 8px;
  padding: 8px 10px;
}
.role-row.me { border-color: var(--el-color-primary); background: var(--el-color-primary-light-9); }
.role-name { display: flex; align-items: center; gap: 8px; }
.me-tag { font-size: 11px; color: var(--el-color-primary); }
.role-desc { margin-top: 4px; line-height: 1.6; }
.scope-text { line-height: 1.7; }
.limits { margin: 0; padding-left: 18px; line-height: 1.8; color: var(--el-text-color-secondary); }

@media (max-width: 599px) {
  .token-meta { margin-left: 0; }
  .search { width: 100%; }
  .ep-summary, .ep-body { padding-left: 0; }
  .copy { position: static; display: block; margin: -4px 0 6px; }
}
</style>
