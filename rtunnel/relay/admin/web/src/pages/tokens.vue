<template>
  <div class="panel">
    <div class="panel-head">
      <h2>API Token</h2>
      <span class="muted">共 {{ list.length }} 个</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索名称 / 账号 / 前缀" clearable class="search" @input="onSearch">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button type="primary" @click="openCreate">+ 新建 Token</el-button>
    </div>

    <div class="tip muted">
      调用管理台接口时加 <code>Authorization: Bearer atk-…</code>。
      Token 继承所属账号的权限组；加入权限组后只能收窄，不会超过归属账号。
    </div>

    <el-table :data="list" v-loading="loading">
      <el-table-column label="名称 / 前缀" :min-width="isPhone ? 150 : 170" prop="name"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <b>{{ row.name || '未命名' }}</b>
          <div class="mono muted">{{ row.token_prefix }}</div>
          <div v-if="isPhone && canListUsers" class="muted">{{ row.username }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="canListUsers && !isPhone" label="所属账号" min-width="120" prop="username"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">{{ row.username }}</template>
      </el-table-column>
      <el-table-column label="状态 / 到期" :min-width="isPhone ? 110 : 140" prop="status"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <el-tag :type="statusType(row.status)" size="small">{{ statusText(row.status) }}</el-tag>
          <div class="muted">{{ expiryText(row.expires_at_ms) }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="权限组" min-width="150" prop="group_count"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">
          <template v-if="row.group_count > 0">
            <el-tag v-for="g in splitGroups(row.groups)" :key="g" size="small" class="gtag">{{ g }}</el-tag>
          </template>
          <span v-else class="muted">全部（继承账号）</span>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="最近使用" min-width="140" prop="last_used_ms"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">
          {{ row.last_used_ms ? time(row.last_used_ms) : '—' }}
          <div v-if="row.last_used_ip" class="muted mono">{{ row.last_used_ip }}</div>
        </template>
      </el-table-column>
      <el-table-column :min-width="isPhone ? 92 : 236" label="操作">
        <template #default="{ row }">
          <template v-if="row.status === 'revoked'">
            <span class="muted">已吊销</span>
          </template>
          <el-dropdown v-else-if="isPhone" trigger="click" @command="(c) => onAction(c, row)">
            <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item command="usage">用法</el-dropdown-item>
                <el-dropdown-item command="edit">编辑</el-dropdown-item>
                <el-dropdown-item command="toggle">{{ row.status === 'active' ? '停用' : '启用' }}</el-dropdown-item>
                <el-dropdown-item command="groups">权限组</el-dropdown-item>
                <el-dropdown-item command="rotate" divided>轮换</el-dropdown-item>
                <el-dropdown-item command="revoke">吊销</el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
          <template v-else>
            <el-button size="small" plain @click="openUsage(row)">用法</el-button>
            <el-button size="small" @click="openEdit(row)">编辑</el-button>
            <el-button size="small" @click="toggleToken(row)">{{ row.status === 'active' ? '停用' : '启用' }}</el-button>
            <el-dropdown trigger="click" @command="(c) => onAction(c, row)">
              <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="groups">权限组</el-dropdown-item>
                  <el-dropdown-item command="rotate" divided>轮换</el-dropdown-item>
                  <el-dropdown-item command="revoke">吊销</el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
          </template>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无 API Token" :image-size="70" /></template>
    </el-table>
  </div>

  <!-- 新建 / 编辑 -->
  <el-dialog v-model="dlg" :title="mode === 'create' ? '新建 API Token' : '编辑 API Token'" width="480px">
    <el-form ref="formRef" :model="form" :rules="rules" label-width="88px">
      <el-form-item v-if="mode === 'create' && canListUsers" label="所属账号">
        <el-select v-model="form.user_id" filterable style="width: 100%">
          <el-option v-for="u in users" :key="u.id" :label="u.username + ' · ' + (ROLE_NAMES[u.role] || u.role)"
                     :value="u.id" />
        </el-select>
      </el-form-item>
      <el-form-item label="名称" prop="name">
        <el-input v-model="form.name" maxlength="64" placeholder="例如 构建机 / 监控采集" />
      </el-form-item>
      <el-form-item v-if="mode === 'create'" label="有效期">
        <el-select v-model="form.expires_in_days" style="width: 100%">
          <el-option v-for="o in expireOptions" :key="o.value" :label="o.label" :value="o.value" />
        </el-select>
      </el-form-item>
      <el-form-item v-else label="到期时间">
        <el-date-picker v-model="form.expires_at" type="datetime" value-format="x"
                        placeholder="留空表示永久" style="width: 100%" />
      </el-form-item>
    </el-form>
    <div class="muted">
      Token 只在创建时显示一次；权限组可在创建后单独设置。
    </div>
    <template #footer>
      <el-button @click="dlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="save">保存</el-button>
    </template>
  </el-dialog>

  <!-- 资源权限组 -->
  <el-dialog v-model="grpDlg" title="资源权限组" width="560px">
    <div class="muted mb8">
      Token 的有效权限 = 归属账号 ∩ 所选的组；不选则完全继承归属账号，永远不会超过归属账号。
    </div>
    <div v-if="!groups.length" class="muted">还没有权限组，先到「权限组」页创建。</div>
    <el-checkbox-group v-else v-model="groupSel" class="scope" :disabled="!canBindGroups">
      <el-checkbox v-for="g in groups" :key="g.id" :value="g.id" class="scope-item">
        <b>{{ g.name }}</b>
        <span class="muted"> · {{ g.super ? '全部资源' : g.resources.length + ' 项资源' }}</span>
        <span class="muted"> · {{ g.super || g.all_devices ? '全部设备' : g.device_count + ' 台' }}</span>
      </el-checkbox>
    </el-checkbox-group>
    <div v-if="!canBindGroups" class="muted mt8">只有管理员可以修改分组绑定。</div>
    <template #footer>
      <el-button @click="grpDlg = false">{{ canBindGroups ? '取消' : '关闭' }}</el-button>
      <el-button v-if="canBindGroups" type="primary" :loading="saving" @click="saveGroups">保存</el-button>
    </template>
  </el-dialog>

  <!-- 一次性明文 -->
  <el-dialog v-model="secretDlg" :title="secretTitle" width="600px">
    <div class="muted">只显示这一次，关闭后无法再次查看；丢失只能轮换。</div>
    <div class="secret mono">{{ secret }}</div>
    <el-button size="small" @click="copyText(secret, 'Token 已复制')">复制 Token</el-button>
    <el-button v-if="secretRow" size="small" text @click="usageFromSecret">查看接口用法</el-button>
    <div class="muted mt10">调用示例：</div>
    <div class="secret mono cmd">curl -H "Authorization: Bearer {{ secret }}" {{ apiBase }}/api/v1/agents</div>
    <template #footer>
      <el-button type="primary" @click="secretDlg = false">我已保存</el-button>
    </template>
  </el-dialog>

  <!-- 用法抽屉：按该 Token 的角色裁剪接口清单 -->
  <el-drawer v-model="usageDlg" :title="usageTitle" :size="isPhone ? '100%' : '720px'" destroy-on-close>
    <ApiDocPanel v-if="usageRow" :token="usageRow" compact />
  </el-drawer>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { auth, can, ROLE_NAMES } from '@/shared/store.js'
import { copyText, time } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import ApiDocPanel from '@/components/ApiDocPanel.vue'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'

const { isPhone } = useBreakpoint()
const canListUsers = computed(() => can('account.read'))
const canBindGroups = computed(() => can('group.member'))

const list = ref([])
const users = ref([])
const q = ref('')
const loading = ref(false)
const saving = ref(false)

const dlg = ref(false)
const mode = ref('create')
const form = reactive({ id: 0, user_id: 0, name: '', expires_in_days: 30, expires_at: '' })
const formRef = ref(null)
const rules = {
  name: [{ required: true, message: '请填写名称', trigger: 'blur' }],
}
const expireOptions = [
  { value: 0, label: '永久' }, { value: 7, label: '7 天' }, { value: 30, label: '30 天' },
  { value: 90, label: '90 天' }, { value: 365, label: '1 年' },
]

const grpDlg = ref(false)
const groups = ref([])
const groupSel = ref([])
const grpToken = ref(0)

const secretDlg = ref(false)
const secret = ref('')
const secretTitle = ref('')
const secretRow = ref(null)
const apiBase = computed(() => location.origin)

const usageDlg = ref(false)
const usageRow = ref(null)
const usageTitle = computed(() =>
  '用法 · ' + ((usageRow.value && usageRow.value.name) || '未命名 Token'))

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
function splitGroups(s) {
  return (s || '').split(',').map((x) => x.trim()).filter(Boolean)
}

let timer = null
async function load() {
  loading.value = true
  try {
    const suffix = q.value.trim() ? '?q=' + encodeURIComponent(q.value.trim()) : ''
    list.value = await Api.get('/api/v1/admin-tokens' + suffix)
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onSearch() {
  clearTimeout(timer)
  timer = setTimeout(load, 250)
}

async function loadUsers() {
  if (!canListUsers.value) return
  try {
    users.value = await Api.get('/api/v1/users')
  } catch (e) {
    users.value = []
  }
}

function openCreate() {
  mode.value = 'create'
  form.id = 0
  form.user_id = (auth.me && auth.me.id) || 0
  form.name = ''
  form.expires_in_days = 30
  form.expires_at = ''
  dlg.value = true
}

function openEdit(row) {
  mode.value = 'edit'
  form.id = row.id
  form.user_id = row.user_id
  form.name = row.name || ''
  form.expires_at = row.expires_at_ms ? String(row.expires_at_ms) : ''
  dlg.value = true
}

async function save() {
  const ok = await formRef.value.validate().catch(() => false)
  if (!ok) return
  saving.value = true
  try {
    if (mode.value === 'create') {
      const body = {
        name: form.name.trim(),
        expires_in_days: Number(form.expires_in_days) || 0,
      }
      if (canListUsers.value && form.user_id) body.user_id = form.user_id
      const r = await Api.post('/api/v1/admin-tokens', body)
      dlg.value = false
      showSecret('API Token 已创建', r.token, r)
    } else {
      await Api.put('/api/v1/admin-tokens/' + form.id, {
        name: form.name.trim(),
        expires_at_ms: Number(form.expires_at) || 0,
      })
      dlg.value = false
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
    await Api.put('/api/v1/admin-tokens/' + row.id, { status: next })
    ElMessage.success(next === 'active' ? '已启用' : '已停用')
    await load()
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

async function rotateToken(row) {
  try {
    await ElMessageBox.confirm('将生成新 Token 并立即吊销旧 Token。', '轮换 API Token',
      { type: 'warning', confirmButtonText: '轮换', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    const r = await Api.post('/api/v1/admin-tokens/' + row.id + '/rotate', {})
    showSecret('API Token 已轮换', r.token, r)
    await load()
  } catch (e) {
    ElMessage.error('轮换失败：' + e.message)
  }
}

async function revokeToken(row) {
  try {
    await ElMessageBox.confirm('吊销后不可恢复，使用该 Token 的调用会立即失败。', '吊销 API Token',
      { type: 'warning', confirmButtonText: '吊销', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/admin-tokens/' + row.id)
    ElMessage.success('已吊销')
    await load()
  } catch (e) {
    ElMessage.error('吊销失败：' + e.message)
  }
}

async function openGroups(row) {
  grpToken.value = row.id
  groups.value = []
  groupSel.value = []
  grpDlg.value = true
  try {
    const r = await Api.get('/api/v1/permission/subjects/atk/' + row.id + '/groups')
    groups.value = r.groups || []
    groupSel.value = r.ids || []
  } catch (e) {
    ElMessage.error('加载权限组失败：' + e.message)
  }
}

async function saveGroups() {
  saving.value = true
  try {
    await Api.put('/api/v1/permission/subjects/atk/' + grpToken.value + '/groups', { ids: groupSel.value })
    ElMessage.success('权限组已更新')
    grpDlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

function showSecret(title, token, row = null) {
  secretTitle.value = title
  secret.value = token
  secretRow.value = row
  secretDlg.value = true
}

function openUsage(row) {
  usageRow.value = row
  usageDlg.value = true
}

function usageFromSecret() {
  const row = secretRow.value
  secretDlg.value = false
  if (row) openUsage(row)
}

function onAction(cmd, row) {
  if (cmd === 'usage') openUsage(row)
  else if (cmd === 'edit') openEdit(row)
  else if (cmd === 'toggle') toggleToken(row)
  else if (cmd === 'groups') openGroups(row)
  else if (cmd === 'rotate') rotateToken(row)
  else if (cmd === 'revoke') revokeToken(row)
}

onMounted(() => {
  load()
  loadUsers()
})
</script>

<style scoped>
.search { width: 240px; }
.tip { margin-bottom: 12px; font-size: 12.5px; line-height: 1.7; }
.tip code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
.mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
.gtag { margin-right: 4px; }
.mb8 { margin-bottom: 8px; }
.mt8 { margin-top: 8px; }
.mt10 { margin-top: 10px; }
.scope { display: flex; flex-direction: column; gap: 4px; }
.scope-item { height: auto; margin-right: 0; }
.secret {
  margin: 10px 0;
  padding: 10px 12px;
  border-radius: 8px;
  background: var(--el-fill-color-light);
  overflow-wrap: anywhere;
  font-size: 12.5px;
}
.secret.cmd { font-size: 11.5px; }
:deep(.el-button + .el-button) { margin-left: 4px; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; row-gap: 8px; }
  .panel-head h2, .panel-head .muted { white-space: nowrap; }
  .search { width: 100%; }
}
</style>
