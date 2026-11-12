<template>
  <div class="panel">
    <div class="panel-head">
      <h2>账号列表</h2>
      <span class="muted">共 {{ users.length }} 个</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索用户名" clearable class="search">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button type="primary" @click="openCreate">+ 新建账号</el-button>
    </div>

    <el-table :data="filtered" v-loading="loading">
      <el-table-column label="用户名" :min-width="isPhone ? 150 : 160">
        <template #default="{ row }">
          <b>{{ row.username }}</b>
          <el-tag v-if="row.username === me.username" size="small" class="ml6">我</el-tag>
          <div v-if="isPhone">
            <el-tag :type="row.status === 'active' ? 'success' : 'info'" size="small">
              {{ row.status === 'active' ? '启用' : '停用' }}
            </el-tag>
          </div>
        </template>
      </el-table-column>
      <el-table-column label="角色" :width="isPhone ? 100 : 130">
        <template #default="{ row }">
          <el-tag :type="roleType(row.role)" size="small">{{ roleName(row.role) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="状态" width="90">
        <template #default="{ row }">
          <el-tag :type="row.status === 'active' ? 'success' : 'info'" size="small">
            {{ row.status === 'active' ? '启用' : '停用' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="创建时间" min-width="150">
        <template #default="{ row }">{{ time(row.created_ms) }}</template>
      </el-table-column>
      <el-table-column :min-width="isPhone ? 92 : 220" label="操作">
        <template #default="{ row }">
          <el-dropdown v-if="isPhone" trigger="click" @command="(c) => onAction(c, row)">
            <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item v-if="canEdit(row)" command="edit">编辑</el-dropdown-item>
                <el-dropdown-item v-if="canManageTarget(row)" command="status">
                  {{ row.status === 'active' ? '停用' : '启用' }}
                </el-dropdown-item>
                <el-dropdown-item v-if="canManageTarget(row)" command="del" divided>删除</el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
          <template v-else>
            <el-button v-if="canEdit(row)" size="small" @click="openEdit(row)">编辑</el-button>
            <el-button v-if="canManageTarget(row)" size="small" @click="toggleStatus(row)">
              {{ row.status === 'active' ? '停用' : '启用' }}
            </el-button>
            <el-button v-if="canManageTarget(row)" size="small" type="danger" plain @click="removeUser(row)">删除</el-button>
          </template>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无账号" :image-size="70" /></template>
    </el-table>

    <!-- 新建 -->
    <el-dialog v-model="createDlg" title="新建账号" width="440px">
      <el-form :model="createForm" label-width="72px">
        <el-form-item label="用户名" required>
          <el-input v-model="createForm.username" maxlength="64" autocomplete="off" />
        </el-form-item>
        <el-form-item label="密码" required>
          <el-input v-model="createForm.password" type="password" show-password autocomplete="new-password"
                    placeholder="至少 8 位" />
        </el-form-item>
        <el-form-item label="角色">
          <el-select v-model="createForm.role" style="width: 100%">
            <el-option v-for="r in roleChoices" :key="r.value" :label="r.label" :value="r.value" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="createUser">创建</el-button>
      </template>
    </el-dialog>

    <!-- 编辑 -->
    <el-dialog v-model="editDlg" :title="'编辑 ' + (edit.username || '')" width="520px">
      <el-form label-width="96px">
        <template v-if="canManageTarget(edit.user)">
          <el-form-item label="角色">
            <el-select v-model="edit.role" style="width: 200px">
              <el-option v-for="r in roleChoices" :key="r.value" :label="r.label" :value="r.value" />
            </el-select>
          </el-form-item>
          <el-form-item label="状态">
            <el-switch v-model="edit.status" active-value="active" inactive-value="disabled"
                       inline-prompt active-text="启用" inactive-text="停用" />
          </el-form-item>
        </template>

        <el-form-item v-if="edit.user && edit.user.username === me.username" label="当前密码">
          <el-input v-model="edit.old_password" type="password" show-password autocomplete="current-password"
                    placeholder="修改自己的密码需先验证" />
        </el-form-item>
        <el-form-item label="新密码">
          <el-input v-model="edit.password" type="password" show-password autocomplete="new-password"
                    placeholder="留空表示不修改，至少 8 位" />
        </el-form-item>

        <el-form-item v-if="showScope" label="设备范围">
          <div v-if="!edit.groups.length" class="muted">还没有设备分组，先到权限页创建。</div>
          <el-checkbox-group v-else v-model="edit.scope" class="scope">
            <el-checkbox v-for="g in edit.groups" :key="g.id" :value="g.id" class="scope-item">
              {{ g.name }} <span class="muted">· {{ g.device_count }} 台</span>
            </el-checkbox>
          </el-checkbox-group>
        </el-form-item>
      </el-form>
      <div class="muted">角色/状态立即生效；新密码至少 8 位，留空不改。</div>
      <template #footer>
        <el-button @click="editDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveUser">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { Api } from '~/shared/api.js'
import { auth, roleOf, ROLE_NAMES } from '~/shared/store.js'
import { time } from '~/shared/utils.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const users = ref([])
const q = ref('')
const loading = ref(false)
const saving = ref(false)
const me = computed(() => auth.me || {})

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  return k ? users.value.filter((u) => u.username.toLowerCase().includes(k)) : users.value
})

const roleChoices = computed(() => {
  if (roleOf() === 'admin') {
    return [
      { value: 'viewer', label: '浏览者' },
      { value: 'operator', label: '操作员' },
      { value: 'manager', label: '管理员' },
    ]
  }
  return [
    { value: 'viewer', label: '浏览者' },
    { value: 'operator', label: '操作员' },
  ]
})

function roleName(r) {
  return ROLE_NAMES[r] || r
}
function roleType(r) {
  return { admin: 'warning', manager: 'primary', operator: 'info', viewer: 'info' }[r] || 'info'
}

function canManageTarget(u) {
  if (!u || u.role === 'admin' || u.username === me.value.username) return false
  if (roleOf() === 'admin') return true
  if (roleOf() === 'manager') return u.role === 'operator' || u.role === 'viewer'
  return false
}
function canEdit(u) {
  return canManageTarget(u) || (u && u.username === me.value.username)
}

const showScope = computed(() =>
  roleOf() === 'admin' && edit.user && edit.user.role !== 'admin')

const createDlg = ref(false)
const createForm = reactive({ username: '', password: '', role: 'viewer' })

const editDlg = ref(false)
const edit = reactive({
  user: null, username: '', role: 'viewer', status: 'active',
  old_password: '', password: '', groups: [], scope: [], initialScope: '[]',
})

async function load() {
  loading.value = true
  try {
    users.value = await Api.get('/api/v1/users')
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function openCreate() {
  createForm.username = ''
  createForm.password = ''
  createForm.role = roleOf() === 'admin' ? 'viewer' : 'operator'
  createDlg.value = true
}

async function createUser() {
  const username = createForm.username.trim()
  if (!username || createForm.password.length < 8) {
    ElMessage.warning('请填写用户名，密码至少 8 位')
    return
  }
  saving.value = true
  try {
    await Api.post('/api/v1/users', { username, password: createForm.password, role: createForm.role })
    ElMessage.success('已创建 ' + username)
    createDlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('创建失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function openEdit(u) {
  Object.assign(edit, {
    user: u, username: u.username, role: u.role, status: u.status,
    old_password: '', password: '', groups: [], scope: [], initialScope: '[]',
  })
  editDlg.value = true
  if (showScope.value) {
    try {
      const r = await Api.get('/api/v1/users/' + u.id + '/groups')
      edit.groups = r.groups || []
      edit.scope = edit.groups.filter((g) => g.selected).map((g) => g.id)
      edit.initialScope = JSON.stringify(edit.scope.slice().sort())
    } catch (e) {
      ElMessage.warning('设备范围加载失败：' + e.message)
    }
  }
}

async function saveUser() {
  const u = edit.user
  if (!u) return
  saving.value = true
  try {
    if (canManageTarget(u)) {
      if (edit.role !== u.role) await Api.put('/api/v1/users/' + u.id + '/role', { role: edit.role })
      if (edit.status !== u.status) await Api.put('/api/v1/users/' + u.id + '/status', { status: edit.status })
    }
    if (edit.password) {
      if (edit.password.length < 8) throw new Error('新密码至少 8 位')
      const body = { new_password: edit.password }
      if (u.username === me.value.username) body.old_password = edit.old_password
      await Api.put('/api/v1/users/' + u.id + '/password', body)
    }
    if (showScope.value) {
      const scope = edit.scope.slice().sort()
      if (JSON.stringify(scope) !== edit.initialScope) {
        await Api.put('/api/v1/users/' + u.id + '/groups', { groups: scope })
      }
    }
    ElMessage.success('已保存')
    editDlg.value = false
    await load()
    if (u.username === me.value.username && edit.password) {
      ElMessage.info('密码已修改，其他会话已失效')
    }
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function toggleStatus(u) {
  const next = u.status === 'active' ? 'disabled' : 'active'
  try {
    await Api.put('/api/v1/users/' + u.id + '/status', { status: next })
    ElMessage.success(next === 'active' ? '已启用' : '已停用')
    await load()
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

async function removeUser(u) {
  try {
    await ElMessageBox.confirm('删除 ' + u.username + '？设备会自动解绑。', '删除账号',
      { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/users/' + u.id)
    ElMessage.success('已删除 ' + u.username)
    await load()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

function onAction(cmd, u) {
  if (cmd === 'edit') openEdit(u)
  else if (cmd === 'status') toggleStatus(u)
  else if (cmd === 'del') removeUser(u)
}

onMounted(load)
</script>

<style scoped>
.search { width: 220px; }
.ml6 { margin-left: 6px; }
.scope { display: flex; flex-direction: column; align-items: flex-start; gap: 2px; }
.scope-item { margin-right: 0; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
