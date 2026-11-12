<template>
  <div v-if="gone" class="panel">
    <el-empty description="账号不存在或已删除">
      <el-button type="primary" @click="$router.replace('/access')">返回接入账号</el-button>
    </el-empty>
  </div>

  <div v-else>
    <div class="back">
      <el-button link @click="$router.push('/access')">← 接入账号</el-button>
    </div>

    <el-alert v-if="acc && acc.status === 'disabled'" type="warning" show-icon :closable="false" class="mb12"
              :title="`账号已停用，名下 ${acc.token_count || 0} 个 Token 均无法接入；重新启用后立即恢复。`" />

    <div class="panel">
      <div class="panel-head">
        <h2>{{ acc ? acc.username : '…' }}</h2>
        <el-tag v-if="acc" :type="acc.status === 'active' ? 'success' : 'info'" size="small">
          {{ acc.status === 'active' ? '启用' : '停用' }}
        </el-tag>
        <span class="spacer" />
        <div class="head-actions">
          <el-button v-if="acc && canManage()" @click="openAccountEdit">编辑资料</el-button>
          <el-button v-if="acc && canManage()" :type="acc.status === 'active' ? 'warning' : 'success'" plain
                     @click="toggleAccount">{{ acc.status === 'active' ? '停用' : '启用' }}</el-button>
          <el-button v-if="acc && isAdmin" type="danger" plain @click="deleteAccount">删除账号</el-button>
        </div>
      </div>

      <el-descriptions v-if="acc" :column="descCols" size="small" border>
        <el-descriptions-item label="展示名">{{ acc.display_name || '—' }}</el-descriptions-item>
        <el-descriptions-item label="账号 ID">{{ acc.id }}</el-descriptions-item>
        <el-descriptions-item label="创建人">{{ acc.created_by || '—' }}</el-descriptions-item>
        <el-descriptions-item label="创建时间">{{ time(acc.created_ms) }}</el-descriptions-item>
        <el-descriptions-item label="最近使用">
          {{ acc.last_used_ms ? time(acc.last_used_ms) + (acc.last_used_ip ? ' · ' + acc.last_used_ip : '') : '—' }}
        </el-descriptions-item>
        <el-descriptions-item label="Token / 分组">
          {{ acc.token_count || 0 }} 个 / {{ acc.group_count || 0 }} 组
        </el-descriptions-item>
        <el-descriptions-item label="备注" :span="descCols">{{ acc.note || '—' }}</el-descriptions-item>
        <el-descriptions-item label="有效权限" :span="descCols">{{ effectiveText }}</el-descriptions-item>
      </el-descriptions>
      <el-skeleton v-else :rows="3" animated />
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>Token</h2>
        <span class="muted">共 {{ tokens.length }} 个</span>
        <span class="spacer" />
        <el-button v-if="canManage()" type="primary" @click="openTokenCreate">+ 新建 Token</el-button>
      </div>

      <el-tabs v-model="filter" class="tok-tabs">
        <el-tab-pane name="all">
          <template #label>全部 <span class="cnt">{{ counts.all }}</span></template>
        </el-tab-pane>
        <el-tab-pane name="active">
          <template #label>启用 <span class="cnt">{{ counts.active }}</span></template>
        </el-tab-pane>
        <el-tab-pane name="disabled">
          <template #label>停用 <span class="cnt">{{ counts.disabled }}</span></template>
        </el-tab-pane>
        <el-tab-pane name="revoked">
          <template #label>已吊销 <span class="cnt">{{ counts.revoked }}</span></template>
        </el-tab-pane>
      </el-tabs>

      <el-table :data="filteredTokens" v-loading="loading">
        <el-table-column label="名称 / 前缀" :min-width="isPhone ? 120 : 160">
          <template #default="{ row }">
            <b>{{ row.name || '未命名' }}</b>
            <div class="mono muted">{{ row.token_prefix }}…</div>
          </template>
        </el-table-column>
        <el-table-column label="状态 / 到期" :min-width="isPhone ? 110 : 140">
          <template #default="{ row }">
            <el-tag :type="statusType(row.status)" size="small">{{ statusText(row.status) }}</el-tag>
            <div class="muted">{{ expiryText(row.expires_at_ms) }}</div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="流量" min-width="170">
          <template #default="{ row }">
            <el-progress v-if="row.quota_bytes > 0" :percentage="quotaPct(row)" :stroke-width="5"
                         :show-text="false" :color="quotaPct(row) >= 100 ? '#e5534b' : quotaPct(row) >= 80 ? '#f5a623' : '#3b82f6'" />
            <div class="muted">{{ quotaText(row) }}</div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="频率 · 并发" min-width="150">
          <template #default="{ row }">
            {{ row.rate_per_min ? row.rate_per_min + '/分' : '频率不限' }} · 并发 {{ row.max_sessions || '不限' }}
            <div class="muted">{{ row.rate_per_min ? '突发 ' + (row.burst || row.rate_per_min) : '无频率限制' }}</div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="最近使用" min-width="150">
          <template #default="{ row }">
            {{ row.last_used_ms ? time(row.last_used_ms) : '—' }}
            <div v-if="row.last_used_ip" class="muted mono">{{ row.last_used_ip }}</div>
          </template>
        </el-table-column>
        <el-table-column :min-width="isPhone ? 92 : 250" label="操作">
          <template #default="{ row }">
            <template v-if="row.status === 'revoked'">
              <span class="muted">—</span>
            </template>
            <el-dropdown v-else-if="isPhone && canManage()" trigger="click"
                         @command="(c) => onTokenAction(c, row)">
              <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="edit">编辑</el-dropdown-item>
                  <el-dropdown-item command="toggle">{{ row.status === 'active' ? '停用' : '启用' }}</el-dropdown-item>
                  <el-dropdown-item command="rotate" divided>轮换</el-dropdown-item>
                  <el-dropdown-item command="revoke">吊销</el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
            <template v-else-if="canManage()">
              <el-button size="small" @click="openTokenEdit(row)">编辑</el-button>
              <el-button size="small" @click="toggleToken(row)">{{ row.status === 'active' ? '停用' : '启用' }}</el-button>
              <el-button size="small" @click="rotateToken(row)">轮换</el-button>
              <el-button size="small" type="danger" plain @click="revokeToken(row)">吊销</el-button>
            </template>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <template #empty><div class="empty-pad muted">暂无 Token</div></template>
      </el-table>
    </div>

    <!-- 编辑账号资料 -->
    <el-dialog v-model="accDlg" title="编辑账号资料" width="440px">
      <el-form :model="accForm" label-width="72px">
        <el-form-item label="展示名">
          <el-input v-model="accForm.display_name" maxlength="128" placeholder="可选" />
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="accForm.note" maxlength="256" placeholder="可选" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="accDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveAccountEdit">保存</el-button>
      </template>
    </el-dialog>

    <!-- 新建 / 编辑 Token -->
    <el-dialog v-model="tokDlg" :title="tokMode === 'create' ? '新建 Token' : '编辑 Token'" width="520px">
      <el-form :model="tokForm" label-width="96px">
        <el-form-item label="名称" required>
          <el-input v-model="tokForm.name" maxlength="64" placeholder="例如 构建机 / 巡检" />
        </el-form-item>
        <el-form-item v-if="tokMode === 'create'" label="有效期">
          <el-select v-model="tokForm.expires_in_days" style="width: 100%">
            <el-option v-for="o in expireOptions" :key="o.value" :label="o.label" :value="o.value" />
          </el-select>
        </el-form-item>
        <el-form-item v-else label="到期时间">
          <el-date-picker v-model="tokForm.expires_at" type="datetime" value-format="x"
                          placeholder="留空表示永久" style="width: 100%" />
        </el-form-item>
        <el-form-item label="流量配额">
          <el-input-number v-model="tokForm.quota_gb" :min="0" :precision="2" :step="1" style="width: 180px" />
          <span class="unit">GB（0 = 不限）</span>
        </el-form-item>
        <el-form-item label="周期">
          <el-select v-model="tokForm.quota_period" style="width: 180px">
            <el-option label="每月" value="monthly" />
            <el-option label="每日" value="daily" />
            <el-option label="总量" value="total" />
          </el-select>
        </el-form-item>
        <el-form-item label="超限动作">
          <el-select v-model="tokForm.quota_action" style="width: 180px">
            <el-option label="断开连接" value="cut" />
            <el-option label="只拒绝新连接" value="block" />
          </el-select>
        </el-form-item>
        <el-form-item label="频率（次/分）">
          <el-input-number v-model="tokForm.rate_per_min" :min="0" :step="1" style="width: 180px" />
        </el-form-item>
        <el-form-item label="突发">
          <el-input-number v-model="tokForm.burst" :min="0" :step="1" style="width: 180px" />
        </el-form-item>
        <el-form-item label="并发会话">
          <el-input-number v-model="tokForm.max_sessions" :min="0" :step="1" style="width: 180px" />
        </el-form-item>
      </el-form>
      <div class="muted">0 表示不限；修改周期会清零本周期已用流量。</div>
      <template #footer>
        <el-button @click="tokDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveToken">保存并生效</el-button>
      </template>
    </el-dialog>

    <!-- 一次性明文 -->
    <el-dialog v-model="secretDlg" :title="secretTitle" width="560px">
      <div class="muted">只显示这一次，关闭后无法再次查看；丢失只能轮换。</div>
      <div class="secret mono">{{ secret }}</div>
      <el-button size="small" @click="copy(secret)">复制 Token</el-button>
      <div class="muted mt10">客户端可直接用 Token 登录：</div>
      <div class="secret mono cmd">rtunnel-client --token {{ secret }} --socks5 127.0.0.1:1080</div>
      <template #footer>
        <el-button type="primary" @click="secretDlg = false">我已保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '~/shared/api.js'
import { canManage, roleOf } from '~/shared/store.js'
import { bytes, time } from '~/shared/utils.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const route = useRoute()
const router = useRouter()
const { isPhone, isTablet } = useBreakpoint()
const descCols = computed(() => (isPhone.value ? 1 : isTablet.value ? 2 : 3))
const isAdmin = computed(() => roleOf() === 'admin')

const id = computed(() => Number(route.params.id))
const acc = ref(null)
const tokens = ref([])
const eff = ref(null)
const gone = ref(false)
const loading = ref(false)
const saving = ref(false)
const filter = ref('all')

const accDlg = ref(false)
const accForm = reactive({ display_name: '', note: '' })

const tokDlg = ref(false)
const tokMode = ref('create')
const tokForm = reactive({
  id: 0, name: '', expires_in_days: 30, expires_at: '', quota_gb: 0,
  quota_period: 'monthly', quota_action: 'cut', rate_per_min: 0, burst: 0, max_sessions: 0,
})
const expireOptions = [
  { value: 0, label: '永久' }, { value: 7, label: '7 天' }, { value: 30, label: '30 天' },
  { value: 90, label: '90 天' }, { value: 365, label: '1 年' },
]

const secretDlg = ref(false)
const secret = ref('')
const secretTitle = ref('')

const counts = computed(() => {
  const c = { all: tokens.value.length, active: 0, disabled: 0, revoked: 0 }
  tokens.value.forEach((t) => { if (c[t.status] != null) c[t.status]++ })
  return c
})
const filteredTokens = computed(() =>
  filter.value === 'all' ? tokens.value : tokens.value.filter((t) => t.status === filter.value))

const effectiveText = computed(() => {
  if (!eff.value) return '—'
  const groups = (eff.value.groups || []).length
  const devices = (eff.value.devices || []).length
  const caps = eff.value.caps || {}
  return '分组 ' + groups + ' · 设备 ' + devices + ' 台 · TCP ' +
    (caps.tcp_ports || '无') + ' · 文件 ' + (caps.file || 'none') + ' · Shell ' +
    (caps.shell === '1' ? '允许' : '禁止')
})

function statusText(s) {
  return { active: '启用', disabled: '停用', revoked: '已吊销' }[s] || s
}
function statusType(s) {
  return { active: 'success', disabled: 'info', revoked: 'danger' }[s] || 'info'
}
function expiryText(ms) {
  if (!ms) return '永久'
  if (Date.now() >= ms) return '已过期'
  return time(ms)
}
function quotaPct(t) {
  const q = Number(t.quota_bytes) || 0
  if (!q) return 0
  return Math.min(100, Math.floor((Number(t.used_bytes) || 0) * 100 / q))
}
function quotaText(t) {
  const used = bytes(t.used_bytes)
  const q = Number(t.quota_bytes) || 0
  if (!q) return used + ' / 不限'
  const period = { total: '总量', daily: '每日', monthly: '每月' }[t.quota_period] || ''
  const gb = (q / 1024 / 1024 / 1024)
  return used + ' / ' + (gb >= 10 ? gb.toFixed(1) : gb.toFixed(2)) + ' GB · ' + period + ' · ' + quotaPct(t) + '%'
}

async function load() {
  loading.value = true
  try {
    acc.value = await Api.get('/api/v1/client-accounts/' + id.value)
    gone.value = false
  } catch (e) {
    if (e.status === 404) {
      gone.value = true
      return
    }
    ElMessage.error('加载失败：' + e.message)
    return
  } finally {
    loading.value = false
  }
  const [tk, ef] = await Promise.all([
    Api.get('/api/v1/client-accounts/' + id.value + '/tokens').catch(() => []),
    Api.get('/api/v1/client-accounts/' + id.value + '/effective').catch(() => null),
  ])
  tokens.value = Array.isArray(tk) ? tk : []
  eff.value = ef
}

function openAccountEdit() {
  accForm.display_name = acc.value.display_name || ''
  accForm.note = acc.value.note || ''
  accDlg.value = true
}

async function saveAccountEdit() {
  saving.value = true
  try {
    await Api.put('/api/v1/client-accounts/' + id.value, {
      display_name: accForm.display_name.trim(),
      note: accForm.note.trim(),
      status: acc.value.status,
    })
    ElMessage.success('已保存')
    accDlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function toggleAccount() {
  const next = acc.value.status === 'active' ? 'disabled' : 'active'
  if (next === 'disabled') {
    try {
      await ElMessageBox.confirm('停用后其全部 Token 立即失效，在线连接会被断开。', '停用账号',
        { type: 'warning', confirmButtonText: '停用', cancelButtonText: '取消' })
    } catch (e) { return }
  }
  try {
    await Api.put('/api/v1/client-accounts/' + id.value, {
      display_name: acc.value.display_name || '',
      note: acc.value.note || '',
      status: next,
    })
    ElMessage.success(next === 'active' ? '已启用' : '已停用，在线连接已断开')
    await load()
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

async function deleteAccount() {
  let value
  try {
    const r = await ElMessageBox.prompt(
      `将删除 ${acc.value.username}：${acc.value.token_count || 0} 个 Token、${acc.value.group_count || 0} 个分组成员关系。此操作不可恢复，请输入用户名确认。`,
      '删除接入账号',
      {
        type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
        inputPlaceholder: acc.value.username,
        inputValidator: (v) => (v === acc.value.username ? true : '用户名不一致'),
      })
    value = r.value
  } catch (e) { return }
  if (value !== acc.value.username) return
  try {
    await Api.del('/api/v1/client-accounts/' + id.value)
    ElMessage.success('已删除 ' + acc.value.username)
    router.replace('/access')
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

function resetTokenForm(t) {
  tokForm.id = t ? t.id : 0
  tokForm.name = t ? (t.name || '') : ''
  tokForm.expires_in_days = 30
  tokForm.expires_at = t && t.expires_at_ms ? String(t.expires_at_ms) : ''
  tokForm.quota_gb = t && t.quota_bytes ? Number((t.quota_bytes / 1024 / 1024 / 1024).toFixed(2)) : 0
  tokForm.quota_period = t ? (t.quota_period || 'monthly') : 'monthly'
  tokForm.quota_action = t ? (t.quota_action || 'cut') : 'cut'
  tokForm.rate_per_min = t ? (t.rate_per_min || 0) : 0
  tokForm.burst = t ? (t.burst || 0) : 0
  tokForm.max_sessions = t ? (t.max_sessions || 0) : 0
}

function openTokenCreate() {
  tokMode.value = 'create'
  resetTokenForm(null)
  tokDlg.value = true
}

function openTokenEdit(row) {
  tokMode.value = 'edit'
  resetTokenForm(row)
  tokDlg.value = true
}

function tokenBody() {
  const body = {
    name: tokForm.name.trim(),
    quota_gb: Number(tokForm.quota_gb) || 0,
    quota_period: tokForm.quota_period,
    quota_action: tokForm.quota_action,
    rate_per_min: Number(tokForm.rate_per_min) || 0,
    burst: Number(tokForm.burst) || 0,
    max_sessions: Number(tokForm.max_sessions) || 0,
  }
  if (tokMode.value === 'create') body.expires_in_days = Number(tokForm.expires_in_days) || 0
  else body.expires_at_ms = Number(tokForm.expires_at) || 0
  return body
}

async function saveToken() {
  if (!tokForm.name.trim()) {
    ElMessage.warning('请填写名称')
    return
  }
  saving.value = true
  try {
    if (tokMode.value === 'create') {
      const r = await Api.post('/api/v1/client-accounts/' + id.value + '/tokens', tokenBody())
      tokDlg.value = false
      showSecret('Token 已创建', r.token)
    } else {
      await Api.put('/api/v1/client-tokens/' + tokForm.id, tokenBody())
      tokDlg.value = false
      ElMessage.success('已保存，立即生效')
    }
    await load()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function toggleToken(row) {
  const next = row.status === 'active' ? 'disabled' : 'active'
  try {
    await Api.put('/api/v1/client-tokens/' + row.id, { status: next })
    ElMessage.success(next === 'active' ? '已启用' : '已停用')
    await load()
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

async function rotateToken(row) {
  try {
    await ElMessageBox.confirm('将生成新 Token 并立即吊销旧 Token，使用旧 Token 的连接会被断开。', '轮换 Token',
      { type: 'warning', confirmButtonText: '轮换', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    const r = await Api.post('/api/v1/client-tokens/' + row.id + '/rotate', {})
    showSecret('Token 已轮换', r.token)
    await load()
  } catch (e) {
    ElMessage.error('轮换失败：' + e.message)
  }
}

async function revokeToken(row) {
  try {
    await ElMessageBox.confirm('吊销后不可恢复，用量记录保留。', '吊销 Token',
      { type: 'warning', confirmButtonText: '吊销', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/client-tokens/' + row.id)
    ElMessage.success('已吊销')
    await load()
  } catch (e) {
    ElMessage.error('吊销失败：' + e.message)
  }
}

function onTokenAction(cmd, row) {
  if (cmd === 'edit') openTokenEdit(row)
  else if (cmd === 'toggle') toggleToken(row)
  else if (cmd === 'rotate') rotateToken(row)
  else if (cmd === 'revoke') revokeToken(row)
}

function showSecret(title, token) {
  secretTitle.value = title
  secret.value = token
  secretDlg.value = true
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('已复制')
  } catch (e) {
    ElMessage.warning('复制失败，请手动选择')
  }
}

watch(id, () => { if (id.value) load() })
onMounted(load)
</script>

<style scoped>
.back { margin-bottom: 10px; }
.head-actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 8px; }
.head-actions .el-button { margin-left: 0; }
.mb12 { margin-bottom: 12px; }
.cnt {
  display: inline-block;
  min-width: 18px;
  padding: 0 6px;
  margin-left: 4px;
  border-radius: 9px;
  background: var(--el-fill-color);
  color: var(--el-text-color-secondary);
  font-size: 11px;
  line-height: 18px;
  text-align: center;
}
.tok-tabs { margin-bottom: 4px; }
.tok-tabs :deep(.el-tabs__header) { margin-bottom: 10px; }
.unit { margin-left: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.secret {
  margin: 10px 0;
  padding: 10px 12px;
  border: 1px dashed var(--el-border-color);
  border-radius: 8px;
  background: var(--el-fill-color-light);
  word-break: break-all;
  font-size: 13px;
}
.secret.cmd { font-size: 12px; }
.mt10 { margin-top: 10px; }
.empty-pad { padding: 24px; text-align: center; }
:deep(.el-table .cell) { white-space: normal; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
}
</style>
