<template>
  <div class="api-docs" :class="{ compact }">
    <div v-if="token" class="token-head">
      <div class="token-title">
        <b>{{ token.name || '未命名 Token' }}</b>
        <span class="mono muted">{{ token.token_prefix }}</span>
      </div>
      <div class="token-meta">
        <el-tag size="small" type="info">{{ token.username }}</el-tag>
        <el-tag size="small">{{ roleName }}</el-tag>
        <el-tag v-if="token.group_count > 0" size="small" type="warning">
          {{ token.group_count }} 个权限组
        </el-tag>
        <el-tag v-else size="small" type="info">继承账号设备范围</el-tag>
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
        <div class="muted">返回 200 表示 Token 可用；401 检查是否停用/过期，403 检查角色和设备范围。</div>

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

        <div class="sec-title">④ WebSocket（文件/服务/终端）</div>
        <div class="code-wrap">
          <pre class="code">{{ wsSnippet }}</pre>
          <el-button size="small" text class="copy" @click="copy(wsSnippet)">复制</el-button>
        </div>
        <div class="muted">
          atk 走查询参数 <code>?atk=</code>，与网页会话使用同一套帧协议。查询串可能进入反向代理日志，建议只在内网或 HTTPS 下使用。
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
          <el-checkbox v-model="onlyMine">只显示我可调用</el-checkbox>
          <span class="muted hidden-tip" v-if="hiddenCount">隐藏 {{ hiddenCount }} 条</span>
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
                <el-tag size="small" :type="roleTagType(d.minRole)" class="role-tag">{{ d.minRole }}</el-tag>
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
        <div class="sec-title">角色能力</div>
        <div class="role-table">
          <div v-for="r in roleRows" :key="r.role" class="role-row" :class="{ me: r.role === role }">
            <div class="role-name">
              <el-tag size="small" :type="roleTagType(r.role)">{{ r.role }}</el-tag>
              <span class="muted">{{ r.label }}</span>
              <span v-if="r.role === role" class="me-tag">当前</span>
            </div>
            <div class="muted role-desc">{{ r.desc }}</div>
          </div>
        </div>

        <div class="sec-title">设备范围</div>
        <div class="muted scope-text">
          <template v-if="token && token.group_count > 0">
            该 Token 绑定 {{ token.group_count }} 个权限组（{{ token.groups || '—' }}），
            设备相关接口只能看到并操作组内设备，即使归属账号是 admin。
          </template>
          <template v-else>
            未绑定分组，继承归属账号：admin / manager 可见全部设备；operator / viewer 只看得到权限组里授权的设备。
          </template>
        </div>

        <div class="sec-title">API Token 不能做的事</div>
        <ul class="limits">
          <li v-for="l in ATK_UNAVAILABLE" :key="l">{{ l }}</li>
          <li>Token 等同账号身份：可读取设置（脱敏）、审计，manager 及以上还能重置下级密码，请按最小权限发放。</li>
        </ul>
      </el-tab-pane>
    </el-tabs>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { ArrowRight, Search } from '@element-plus/icons-vue'
import { copyText } from '@/shared/utils.js'
import { ROLE_NAMES } from '@/shared/store.js'
import {
  ATK_UNAVAILABLE, DOCS, ERRORS, GROUPS, docsForRole,
} from '@/shared/api-docs.js'

const props = defineProps({
  role: { type: String, default: 'viewer' },
  token: { type: Object, default: null },
  compact: { type: Boolean, default: false },
})

const tab = ref('start')
const q = ref('')
const onlyMine = ref(true)
const openGroups = ref(new Set(['device']))
const expanded = ref(new Set())

const roleName = computed(() => `${props.role} · ${ROLE_NAMES[props.role] || ''}`)

const baseDocs = computed(() =>
  onlyMine.value ? docsForRole(props.role) : DOCS.slice())

const hiddenCount = computed(() => DOCS.length - docsForRole(props.role).length)

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
  return `${proto}://${location.host}/ws/fs/<devid>?atk=$ATK\n` +
    `# 另：/ws/svc/<devid>、/ws/tty/<devid>/<alias>、/ws/proxy/<devid>/<port>`
})

const recipes = [
  { name: '列设备', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents"` },
  { name: '看资源占用', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/sysinfo?mode=inc"` },
  { name: '上传固件', curl: `curl -k -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @fw.bin \\\n  "$RT/api/v1/files/fw.bin"` },
  { name: '分发到多台', curl: `curl -k -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"file":"fw.bin","devids":["mp135","mp136"],"dir":"firmware"}' \\\n  "$RT/api/v1/distributions"` },
  { name: '查审计', curl: `curl -k -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/audit"` },
]

const roleRows = [
  { role: 'viewer', label: '浏览者', desc: '概览、设备列表/详情、sysinfo、指标、串口列表、个人偏好' },
  { role: 'operator', label: '操作员', desc: '在只读基础上增加：会话、客户端、文件列表/下载/上传、设备 HTTP 反代、ACL 读' },
  { role: 'manager', label: '管理员', desc: '增加：设备能力/串口/指标配置、文件写删、分发/OTA、权限与接入账号写、账号管理、审计、证书列表' },
  { role: 'admin', label: '超级管理员', desc: '增加：系统设置、告警、证书签发、ACL 导入、关机；shell 能力仅 admin 可开' },
]

function roleTagType(r) {
  return { viewer: 'info', operator: '', manager: 'success', admin: 'danger' }[r] || 'info'
}

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
