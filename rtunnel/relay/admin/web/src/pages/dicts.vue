<template>
  <div class="panel">
    <div class="panel-head">
      <h2>字典库</h2>
      <span class="muted">资源键、端口与串口字典；权限组按这里的资源授权</span>
      <span class="spacer" />
      <el-button size="small" :loading="loading" @click="load">刷新</el-button>
    </div>

    <el-tabs v-model="tab" @tab-change="onTab">
      <el-tab-pane label="功能资源" name="resources" />
      <el-tab-pane label="设备资源" name="devices" />
      <el-tab-pane label="端口字典" name="ports" />
      <el-tab-pane label="串口字典" name="ttys" />
    </el-tabs>

    <div class="toolbar">
      <el-input v-model="q" size="small" clearable class="w220" placeholder="搜索键/值/名称"
                @keyup.enter="search" @clear="search" />
      <el-button size="small" @click="search">搜索</el-button>
      <span class="spacer" />
      <el-button v-if="tab === 'devices'" size="small" :loading="syncing" @click="syncDevices">同步设备</el-button>
      <el-button v-if="tab === 'devices' && canWrite" size="small" type="primary" @click="addDevice">+ 登记设备</el-button>
      <template v-if="tab === 'ports' || tab === 'ttys'">
        <el-button v-if="canWrite" size="small" @click="batchAdd">批量添加</el-button>
        <el-button v-if="canWrite" size="small" type="primary" @click="openAdd">+ 新增</el-button>
      </template>
      <span v-else-if="tab === 'resources'" class="muted small">资源键由程序登记，可改显示名、说明与启用状态</span>
      <span v-else class="muted small">设备由 agent 上线自动登记，可改名或临时停用</span>
    </div>

    <el-table :data="rows" size="small" v-loading="loading" row-key="id">
      <template v-if="tab === 'resources'">
        <el-table-column label="资源键" min-width="170" prop="key"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><code class="key">{{ row.key }}</code></template>
        </el-table-column>
        <el-table-column label="分类" width="90" prop="category"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.category || '—' }}</template>
        </el-table-column>
        <el-table-column label="显示名" min-width="150" prop="name"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.name || '—' }}</template>
        </el-table-column>
        <el-table-column label="说明" min-width="220" show-overflow-tooltip prop="description"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><span class="muted">{{ row.description || '—' }}</span></template>
        </el-table-column>
        <el-table-column label="状态" width="80" prop="enabled"
                       sortable :sort-by="(r) => (r.enabled ? 1 : 0)" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '停用' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="80" fixed="right">
          <template #default="{ row }">
            <el-button v-if="canWrite" size="small" link type="primary" @click="openEdit(row)">编辑</el-button>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
      </template>

      <template v-else-if="tab === 'devices'">
        <el-table-column label="设备 ID" min-width="150" prop="devid"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><code class="key">{{ row.devid }}</code></template>
        </el-table-column>
        <el-table-column label="名称" min-width="130" prop="name"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.name || '—' }}</template>
        </el-table-column>
        <el-table-column label="版本" width="90" prop="version"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.version || '—' }}</template>
        </el-table-column>
        <el-table-column label="状态" width="90" prop="online"
                       sortable :sort-by="(r) => (r.online ? 1 : 0)" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <el-tag :type="row.online ? 'success' : 'info'" size="small">{{ row.online ? '在线' : '离线' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="租户" min-width="110" prop="tenant_name"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.tenant_name || '默认租户' }}</template>
        </el-table-column>
        <el-table-column label="所属组" width="80" prop="group_count"
                       sortable :sort-orders="NUM_ORDERS">
          <template #default="{ row }">{{ row.group_count || 0 }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="最近在线" min-width="140" prop="last_seen_ms"
                       sortable :sort-orders="NUM_ORDERS">
          <template #default="{ row }"><span class="muted">{{ time(row.last_seen_ms) }}</span></template>
        </el-table-column>
        <el-table-column label="授权" width="80" prop="enabled"
                       sortable :sort-by="(r) => (r.enabled ? 1 : 0)" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '停用' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" :width="canWrite ? 120 : 60" fixed="right">
          <template #default="{ row }">
            <template v-if="canWrite">
              <el-button size="small" link type="primary" @click="openEdit(row)">编辑</el-button>
              <el-button size="small" link @click="toggleDevice(row)">{{ row.enabled ? '停用' : '启用' }}</el-button>
            </template>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
      </template>

      <template v-else>
        <el-table-column :label="tab === 'ports' ? '端口/范围' : '路径/模式'" min-width="170" prop="value"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><code class="key">{{ row.value }}</code></template>
        </el-table-column>
        <el-table-column v-if="tab === 'ttys'" label="类型" width="90" prop="kind"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">{{ row.kind || '—' }}</template>
        </el-table-column>
        <el-table-column label="名称" min-width="130">
          <template #default="{ row }">{{ row.name || '—' }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="说明" min-width="200" show-overflow-tooltip prop="description"
                       sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><span class="muted">{{ row.description || '—' }}</span></template>
        </el-table-column>
        <el-table-column label="状态" width="80">
          <template #default="{ row }">
            <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '停用' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" :width="canWrite ? 120 : 60" fixed="right">
          <template #default="{ row }">
            <template v-if="canWrite">
              <el-button size="small" link type="primary" @click="openEdit(row)">编辑</el-button>
              <el-button size="small" link type="danger" @click="remove(row)">删除</el-button>
            </template>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
      </template>
      <template #empty><div class="muted empty">暂无字典项</div></template>
    </el-table>

    <div class="pager">
      <el-pagination background layout="total, sizes, prev, pager, next" :total="total"
                     :current-page="page" :page-size="size" :page-sizes="[5, 10, 20, 50]"
                     @current-change="onPage" @size-change="onSize" />
    </div>

    <el-dialog v-model="dlg" :title="dlgTitle" width="min(92vw, 520px)">
      <el-form ref="formRef" :model="form" :rules="rules" label-width="76px" size="small" @submit.prevent>
        <el-form-item v-if="tab === 'resources'" label="资源键">
          <el-input v-model="form.key" disabled />
        </el-form-item>
        <el-form-item v-else-if="tab === 'devices'" label="设备 ID">
          <el-input v-model="form.devid" disabled />
        </el-form-item>
        <el-form-item v-else :label="tab === 'ports' ? '端口' : '路径'" prop="value">
          <el-input v-model="form.value" :disabled="!!form.id"
                    :placeholder="tab === 'ports' ? '22 或 8000-8100' : '/dev/ttyS0 或 ttyUSB*'" />
        </el-form-item>
        <el-form-item label="名称">
          <el-input v-model="form.name" maxlength="64" placeholder="可选" />
        </el-form-item>
        <el-form-item v-if="tab !== 'devices'" label="说明">
          <el-input v-model="form.description" maxlength="128" placeholder="可选" />
        </el-form-item>
        <el-form-item v-if="tab === 'devices'" label="租户">
          <el-select v-model="form.tenant_id" style="width: 100%">
            <el-option v-for="t in tenants" :key="t.id" :label="t.name" :value="t.id" />
          </el-select>
        </el-form-item>
        <el-form-item :label="tab === 'devices' ? '允许授权' : '启用'">
          <el-switch v-model="form.enabled" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button size="small" @click="dlg = false">取消</el-button>
        <el-button size="small" type="primary" :loading="saving" @click="save">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { time } from '@/shared/utils.js'
import { can } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'

const { isPhone } = useBreakpoint()
const canWrite = computed(() => can('dict.write'))

const tab = ref('resources')
const syncing = ref(false)
const q = ref('')
const page = ref(1)
const size = ref(10)
const total = ref(0)
const rows = ref([])
const loading = ref(false)

const dlg = ref(false)
const saving = ref(false)
const formRef = ref(null)
const form = reactive({ id: 0, key: '', value: '', devid: '', name: '', description: '', enabled: true, tenant_id: 1 })
const tenants = ref([])
const rules = {
  value: [{
    validator: (rule, v, cb) => {
      if (form.id || String(v || '').trim()) return cb()
      return cb(new Error(tab.value === 'ports' ? '请填写端口' : '请填写串口路径'))
    },
    trigger: 'blur',
  }],
}

const dlgTitle = computed(() => {
  if (tab.value === 'resources') return '编辑资源'
  if (tab.value === 'devices') return '编辑设备资源'
  return (form.id ? '编辑' : '新增') + (tab.value === 'ports' ? '端口' : '串口')
})

function query() {
  const p = new URLSearchParams({
    kind: tab.value,
    page: String(page.value),
    page_size: String(size.value),
  })
  const kw = q.value.trim()
  if (kw) p.set('q', kw)
  return p.toString()
}

async function load() {
  loading.value = true
  try {
    if (tab.value === 'devices' && !tenants.value.length) {
      const t = await Api.get('/api/v1/tenants').catch(() => null)
      tenants.value = (t && t.items) || []
    }
    const r = await Api.get('/api/v1/permission/dicts?' + query())
    rows.value = (r && r.items) || []
    total.value = Number(r && r.total) || 0
    // 删除最后一页最后一条后自动回退
    const maxPage = Math.max(1, Math.ceil(total.value / size.value))
    if (page.value > maxPage) {
      page.value = maxPage
      return load()
    }
  } catch (e) {
    rows.value = []
    total.value = 0
    ElMessage.error('字典加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onTab() {
  page.value = 1
  q.value = ''
  load()
}

function search() {
  page.value = 1
  load()
}

function onPage(p) {
  page.value = p
  load()
}

function onSize(s) {
  size.value = s
  page.value = 1
  load()
}

function openAdd() {
  Object.assign(form, { id: 0, key: '', value: '', devid: '', name: '', description: '', enabled: true, tenant_id: 1 })
  dlg.value = true
}

function openEdit(row) {
  Object.assign(form, {
    id: row.id || 0,
    key: row.key || '',
    value: row.value || '',
    devid: row.devid || '',
    name: row.name || '',
    description: row.description || '',
    enabled: !!row.enabled,
    tenant_id: row.tenant_id || 1,
  })
  dlg.value = true
}

async function save() {
  if (tab.value === 'ports' || tab.value === 'ttys') {
    const ok = await formRef.value.validate().catch(() => false)
    if (!ok) return
  }
  saving.value = true
  try {
    if (tab.value === 'resources') {
      await Api.put('/api/v1/permission/resources/' + encodeURIComponent(form.key), {
        name: form.name.trim(),
        description: form.description.trim(),
        enabled: !!form.enabled,
      })
    } else if (tab.value === 'devices') {
      await Api.put('/api/v1/permission/devices/' + encodeURIComponent(form.devid), {
        name: form.name.trim(),
        enabled: !!form.enabled,
        tenant_id: form.tenant_id,
      })
    } else {
      const kind = tab.value
      const body = {
        name: form.name.trim(),
        description: form.description.trim(),
        enabled: !!form.enabled,
      }
      if (form.id) {
        await Api.put('/api/v1/permission/dict/' + kind + '/' + form.id, body)
      } else {
        const r = await Api.post('/api/v1/permission/dict/' + kind, {
          value: form.value.trim(),
          name: body.name,
          description: body.description,
        })
        // 新建接口默认启用，按弹窗里的开关补一次状态
        if (!body.enabled && r && r.id) {
          await Api.put('/api/v1/permission/dict/' + kind + '/' + r.id, body)
        }
      }
    }
    dlg.value = false
    ElMessage.success('已保存')
    await load()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function batchAdd() {
  const label = tab.value === 'ports' ? '端口（逗号或换行分隔）' : '串口路径（逗号或换行分隔）'
  let text = ''
  try {
    const r = await ElMessageBox.prompt(label, '批量添加', {
      inputType: 'textarea',
      confirmButtonText: '添加',
      cancelButtonText: '取消',
    })
    text = r.value || ''
  } catch (e) { return }
  const values = text.split(/[\s,;]+/).map((s) => s.trim()).filter(Boolean)
  if (!values.length) {
    ElMessage.warning('没有可添加的值')
    return
  }
  try {
    const r = await Api.post('/api/v1/permission/dict/' + tab.value + '/batch', { values })
    const added = r && Array.isArray(r.added) ? r.added.length : values.length
    ElMessage.success('已添加 ' + added + ' 项')
    await load()
  } catch (e) {
    ElMessage.error('添加失败：' + e.message)
  }
}

async function remove(row) {
  try {
    await ElMessageBox.confirm('删除「' + row.value + '」？被 ACL 引用时无法删除。', '删除字典项', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/permission/dict/' + tab.value + '/' + row.id)
    ElMessage.success('已删除')
    await load()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

async function toggleDevice(row) {
  try {
    await Api.put('/api/v1/permission/devices/' + encodeURIComponent(row.devid), {
      name: row.name || '', enabled: !row.enabled,
    })
    ElMessage.success(row.enabled ? '已停用' : '已启用')
    await load()
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

async function addDevice() {
  let devid = ''
  try {
    const r = await ElMessageBox.prompt('输入设备 ID（agent 的 --devid）', '登记设备资源', {
      confirmButtonText: '登记', cancelButtonText: '取消',
      inputPattern: /^[A-Za-z0-9._:-]{1,128}$/,
      inputErrorMessage: '只允许字母、数字、点、下划线、冒号和短横线',
    })
    devid = (r.value || '').trim()
  } catch (e) { return }
  if (!devid) return
  try {
    await Api.put('/api/v1/permission/devices/' + encodeURIComponent(devid), { name: '', enabled: true })
    ElMessage.success('已登记')
    await load()
  } catch (e) {
    ElMessage.error('登记失败：' + e.message)
  }
}

async function syncDevices() {
  syncing.value = true
  try {
    await Api.get('/api/v1/agents').catch(() => [])
    await load()
    ElMessage.success('设备已同步')
  } finally {
    syncing.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.toolbar { display: flex; align-items: center; gap: 8px; margin: 4px 0 10px; flex-wrap: wrap; }
.toolbar .w220 { width: 220px; }
.toolbar .spacer { flex: 1; }
.small { font-size: 12px; }
.key { font-size: 12px; padding: 1px 5px; border-radius: 4px; background: var(--el-fill-color-light); }
.empty { padding: 18px 4px; }
.pager { display: flex; justify-content: flex-end; margin-top: 12px; }
@media (max-width: 599px) {
  .panel-head h2 { white-space: nowrap; }
  .panel-head > .muted { flex: 1 0 100%; order: 3; }
  .toolbar .w220 { width: 100%; }
  .toolbar .spacer { display: none; }
  .pager { justify-content: center; }
  .pager :deep(.el-pagination__total) { display: none; }
}
</style>
