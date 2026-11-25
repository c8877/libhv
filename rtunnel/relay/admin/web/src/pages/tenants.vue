<template>
  <div class="panel">
    <div class="panel-head">
      <h2>租户</h2>
      <span class="muted">共 {{ list.length }} 个 · 计费与设备归属边界</span>
      <span class="spacer" />
      <el-button v-if="can('tenant.write')" type="primary" @click="openCreate">+ 新建租户</el-button>
    </div>

    <el-table :data="list" v-loading="loading">
      <el-table-column label="租户" :min-width="isPhone ? 150 : 200" prop="name"
                       sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <b>{{ row.name }}</b>
          <el-tag v-if="row.id === 1" size="small" class="ml6">默认</el-tag>
          <div class="muted">{{ row.note || '—' }}</div>
        </template>
      </el-table-column>
      <el-table-column label="状态" width="90" prop="status" sortable :sort-orders="TEXT_ORDERS">
        <template #default="{ row }">
          <el-tag :type="row.status === 'active' ? 'success' : 'info'" size="small">
            {{ row.status === 'active' ? '启用' : '停用' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="账号" width="80" align="right" prop="accounts"
                       sortable :sort-orders="NUM_ORDERS" />
      <el-table-column label="设备" width="110" align="right" prop="devices"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">{{ row.devices || 0 }}<span class="muted"> · 在线 {{ row.online || 0 }}</span></template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="套餐" min-width="200">
        <template #default="{ row }">
          <div>设备 {{ row.max_devices ? (row.devices || 0) + '/' + row.max_devices : '不限' }}</div>
          <div class="muted">
            并发 {{ row.max_sessions || '不限' }} · 月流量
            {{ row.quota_bytes ? fmtBytes(row.used_bytes_month || 0) + '/' + fmtBytes(row.quota_bytes) : '不限' }}
          </div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="创建时间" min-width="150" prop="created_ms"
                       sortable :sort-orders="NUM_ORDERS">
        <template #default="{ row }">{{ row.created_ms ? time(row.created_ms) : '—' }}</template>
      </el-table-column>
      <el-table-column label="操作" width="90">
        <template #default="{ row }">
          <el-button v-if="can('tenant.write')" link type="primary" @click="openEdit(row)">编辑</el-button>
          <span v-else class="muted">—</span>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无租户" :image-size="70" /></template>
    </el-table>

    <div class="hint muted">
      设备与接入账号都归属一个租户；接入令牌只能访问本租户的设备，用量按租户归集。
      停用租户等价于停用其下全部接入账号，在线连接会被断开。默认租户不可停用。
      套餐的设备数按已登记设备计（默认租户不受限），并发与月流量由 relay 在数据面强制；
      超出流量会断开该租户全部令牌，并发/流量到顶后拒绝新会话。
    </div>

    <el-dialog v-model="createDlg" title="新建租户" width="440px">
      <el-form ref="createRef" :model="createForm" :rules="rules" label-width="72px" @submit.prevent>
        <el-form-item label="名称" prop="name">
          <el-input v-model="createForm.name" maxlength="64" placeholder="例如 某客户 / 某产线" />
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="createForm.note" maxlength="256" placeholder="可选" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="create">创建</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="editDlg" :title="'编辑 ' + (edit.name || '')" width="440px">
      <el-form ref="editRef" :model="edit" :rules="rules" label-width="72px" @submit.prevent>
        <el-form-item label="名称" prop="name">
          <el-input v-model="edit.name" maxlength="64" />
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="edit.note" maxlength="256" placeholder="可选" />
        </el-form-item>
        <el-form-item label="状态">
          <el-switch v-model="edit.status" active-value="active" inactive-value="disabled"
                     :disabled="edit.id === 1" inline-prompt active-text="启用" inactive-text="停用" />
        </el-form-item>
        <el-form-item label="设备上限">
          <el-input-number v-model="edit.max_devices" :min="0" :step="1" style="width: 140px" />
          <span class="unit">台（0 = 不限）</span>
        </el-form-item>
        <el-form-item label="并发上限">
          <el-input-number v-model="edit.max_sessions" :min="0" :step="1" style="width: 140px" />
          <span class="unit">会话（0 = 不限）</span>
        </el-form-item>
        <el-form-item label="月流量">
          <el-input-number v-model="edit.quota_gb" :min="0" :precision="2" :step="1" style="width: 140px" />
          <span class="unit">GB/月（0 = 不限）</span>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="editDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveEdit">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { onMounted, reactive, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { time } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'

const { isPhone } = useBreakpoint()

const list = ref([])
const loading = ref(false)
const saving = ref(false)

const createDlg = ref(false)
const createRef = ref(null)
const createForm = reactive({ name: '', note: '' })

const editDlg = ref(false)
const editRef = ref(null)
const edit = reactive({ id: 0, name: '', note: '', status: 'active',
  max_devices: 0, max_sessions: 0, quota_gb: 0 })

const rules = {
  name: [{ required: true, message: '请填写租户名', trigger: 'blur' }],
}

async function load() {
  loading.value = true
  try {
    const res = await Api.get('/api/v1/tenants')
    list.value = (res && res.items) || []
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function fmtBytes(n) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let v = Number(n) || 0
  let i = 0
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return (i === 0 ? String(v) : v.toFixed(v >= 100 ? 0 : 1)) + ' ' + units[i]
}

function openCreate() {
  createForm.name = ''
  createForm.note = ''
  createDlg.value = true
}

async function create() {
  const ok = await createRef.value.validate().catch(() => false)
  if (!ok) return
  saving.value = true
  try {
    await Api.post('/api/v1/tenants', {
      name: createForm.name.trim(),
      note: createForm.note.trim(),
    })
    ElMessage.success('已创建 ' + createForm.name.trim())
    createDlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('创建失败：' + e.message)
  } finally {
    saving.value = false
  }
}

function openEdit(row) {
  Object.assign(edit, {
    id: row.id,
    name: row.name || '',
    note: row.note || '',
    status: row.status || 'active',
    max_devices: row.max_devices || 0,
    max_sessions: row.max_sessions || 0,
    quota_gb: row.quota_bytes ? Number((row.quota_bytes / 1073741824).toFixed(2)) : 0,
  })
  editDlg.value = true
}

async function saveEdit() {
  const ok = await editRef.value.validate().catch(() => false)
  if (!ok) return
  if (edit.status === 'disabled' && edit.id !== 1) {
    try {
      await ElMessageBox.confirm('停用后该租户下的接入账号全部失效，在线连接会被断开。', '停用租户',
        { type: 'warning', confirmButtonText: '停用', cancelButtonText: '取消' })
    } catch (e) { return }
  }
  saving.value = true
  try {
    await Api.put('/api/v1/tenants/' + edit.id, {
      name: edit.name.trim(),
      note: edit.note.trim(),
      status: edit.status,
      max_devices: Number(edit.max_devices) || 0,
      max_sessions: Number(edit.max_sessions) || 0,
      quota_gb: Number(edit.quota_gb) || 0,
    })
    ElMessage.success('已保存')
    editDlg.value = false
    await load()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.ml6 { margin-left: 6px; }
.unit { margin-left: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.hint { margin-top: 10px; font-size: 12.5px; line-height: 1.7; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
}
</style>
