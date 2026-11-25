<template>
  <div class="panel">
    <div class="panel-head">
      <h2>接入账号</h2>
      <span class="muted">共 {{ list.length }} 个</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索用户名 / 展示名" clearable class="search" @input="onSearch">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button v-if="canCreate" type="primary" @click="openCreate">+ 新建账号</el-button>
    </div>

    <el-table :data="list" v-loading="loading" row-class-name="row-click" @row-click="openDetail">
      <el-table-column label="用户名" :min-width="isPhone ? 150 : 180" prop="username"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <b>{{ row.username }}</b>
          <div v-if="row.display_name" class="muted">{{ row.display_name }}</div>
          <div v-if="isPhone" class="muted">{{ tenantName(row.tenant_id) }} · {{ row.token_count || 0 }} Token</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="租户" min-width="120" prop="tenant_name"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">{{ tenantName(row.tenant_id) }}</template>
      </el-table-column>
      <el-table-column label="状态" width="90" prop="status"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <el-tag :type="row.status === 'active' ? 'success' : 'info'" size="small">
            {{ row.status === 'active' ? '启用' : '停用' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="Token" width="90" align="right" prop="token_count"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">{{ row.token_count || 0 }}</template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="最近使用" min-width="160" prop="last_used_ms"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">{{ row.last_used_ms ? time(row.last_used_ms) : '—' }}</template>
      </el-table-column>
      <el-table-column label="操作" width="100">
        <template #default="{ row }">
          <el-button link type="primary" @click.stop="openDetail(row)">详情 ›</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无接入账号" :image-size="70" /></template>
    </el-table>

    <el-dialog v-model="dlg" title="新建接入账号" width="440px">
      <el-form ref="formRef" :model="form" :rules="rules" label-width="72px" @submit.prevent>
        <el-form-item label="用户名" prop="username">
          <el-input v-model="form.username" maxlength="64" placeholder="与客户端 --user 一致" />
        </el-form-item>
        <el-form-item label="租户">
          <el-select v-model="form.tenant_id" style="width: 100%">
            <el-option v-for="t in tenants" :key="t.id" :label="t.name" :value="t.id" />
          </el-select>
        </el-form-item>
        <el-form-item label="展示名">
          <el-input v-model="form.display_name" maxlength="128" placeholder="可选" />
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="form.note" maxlength="256" placeholder="可选" />
        </el-form-item>
      </el-form>
      <div class="muted">账号创建后加入权限组并授权设备资源，才会获得对应访问权限。</div>
      <template #footer>
        <el-button @click="dlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="create">创建</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { can, isSuper } from '@/shared/store.js'
import { time } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'
import { useTenants } from '@/shared/tenants.js'

const { isPhone } = useBreakpoint()

// 新建账号与“账号写”权限一致；超管始终可见
const canCreate = computed(() => isSuper() || can('account.write'))

const router = useRouter()
const list = ref([])
const { tenants, loadTenants, tenantName } = useTenants()
const q = ref('')
const loading = ref(false)
const dlg = ref(false)
const saving = ref(false)
const formRef = ref(null)
const form = reactive({ username: '', display_name: '', note: '', tenant_id: 1 })
const rules = {
  username: [{ required: true, message: '请填写用户名', trigger: 'blur' }],
}

let timer = null
async function load() {
  loading.value = true
  try {
    const suffix = q.value.trim() ? '?q=' + encodeURIComponent(q.value.trim()) : ''
    list.value = await Api.get('/api/v1/client-accounts' + suffix)
    await loadTenants()
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

function openDetail(row) {
  router.push('/access/' + row.id)
}

function openCreate() {
  form.username = ''
  form.display_name = ''
  form.note = ''
  form.tenant_id = tenants.value.length ? tenants.value[0].id : 1
  dlg.value = true
}

async function create() {
  const ok = await formRef.value.validate().catch(() => false)
  if (!ok) return
  const username = form.username.trim()
  saving.value = true
  try {
    await Api.post('/api/v1/client-accounts', {
      username,
      display_name: form.display_name.trim(),
      note: form.note.trim(),
      tenant_id: form.tenant_id,
    })
    ElMessage.success('已创建 ' + username)
    dlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('创建失败：' + e.message)
  } finally {
    saving.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.search { width: 240px; }
:deep(.row-click) { cursor: pointer; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
