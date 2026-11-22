<template>
  <div class="panel">
    <div class="panel-head">
      <h2>授权对象</h2>
      <span class="muted">按对象查看并分配权限组；atk / rtk 的权限不会超过归属账号</span>
      <span class="spacer" />
      <el-input v-model="q" size="small" placeholder="搜索名称或归属" clearable class="w220" />
    </div>

    <el-tabs v-model="tab" class="subj-tabs">
      <el-tab-pane v-for="t in TABS" :key="t.key" :label="t.label + ' (' + countOf(t.key) + ')'" :name="t.key" />
    </el-tabs>
    <div class="subj-hint muted">{{ TAB_HINTS[tab] }}</div>

    <DataTable :data="rows" :sort-keys="sortKeys" size="small" row-key="key" :show-header="!isPhone">
      <el-table-column label="对象" min-width="170">
        <template #default="{ row }">
          <b>{{ row.name }}</b>
          <div class="sub">{{ row.detail || row.type }}</div>
        </template>
      </el-table-column>
      <el-table-column label="类型" width="100">
        <template #default="{ row }">
          <el-tag size="small" effect="plain" :type="tagType(row.type)">{{ typeName(row.type) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="所属权限组" min-width="220">
        <template #default="{ row }">
          <span v-if="row.super" class="muted">超级管理员（全部资源）</span>
          <template v-else-if="row.groups && row.groups.length">
            <el-tag v-for="g in row.groups" :key="g.id" size="small" class="gtag">{{ g.name }}</el-tag>
          </template>
          <span v-else class="muted">{{ row.type === 'user' || row.type === 'atk' ? '未分配（按角色回落）' : '未分配' }}</span>
        </template>
      </el-table-column>
      <el-table-column label="有效资源" width="110">
        <template #default="{ row }">
          <span v-if="row.super || row.all_devices" class="num">{{ row.super ? '全部' : row.resource_count }}</span>
          <span v-else class="num">{{ row.resource_count }} 项</span>
        </template>
      </el-table-column>
      <el-table-column label="设备范围" width="120">
        <template #default="{ row }">
          <span class="num">{{ row.super || row.all_devices ? '全部' : row.device_count + ' 台' }}</span>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="状态" width="80">
        <template #default="{ row }">
          <el-tag size="small" :type="row.status === 'active' ? 'success' : 'info'">
            {{ row.status === 'active' ? '启用' : row.status }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="110" fixed="right">
        <template #default="{ row }">
          <el-button size="small" link type="primary" :disabled="!canWrite" @click="openEdit(row)">权限组</el-button>
        </template>
      </el-table-column>
      <template #empty><span class="muted">没有匹配的对象</span></template>
    </DataTable>

    <el-dialog v-model="dlg" :title="'分配权限组 · ' + edit.name" :width="isPhone ? '92%' : '560px'">
      <div class="dlg-tip muted">
        <template v-if="edit.type === 'atk' || edit.type === 'rtk'">
          令牌权限 = 归属账号权限 ∩ 所选权限组；不选 = 完全继承归属账号。
        </template>
        <template v-else>
          有效权限 = 所选权限组并集；清空后不再按角色回落。
        </template>
      </div>
      <div v-if="!availableGroups.length" class="muted pad">
        {{ edit.type === 'atk' || edit.type === 'rtk' ? '归属账号还没有权限组，先给归属账号分配。' : '还没有权限组，先到「权限组」页创建。' }}
      </div>
      <el-checkbox-group v-else v-model="edit.ids" class="grp-list">
        <el-checkbox v-for="g in availableGroups" :key="g.id" :value="g.id" class="grp-item">
          <b>{{ g.name }}</b>
          <span class="muted">{{ g.super ? '全部资源' : g.resources.length + ' 项资源' }}</span>
          <span class="muted">· {{ g.all_devices ? '全部设备' : g.device_count + ' 台' }}</span>
          <el-tag v-if="g.builtin_key" size="small" effect="plain" class="ml6">预置</el-tag>
        </el-checkbox>
      </el-checkbox-group>
      <template #footer>
        <el-button size="small" @click="dlg = false">取消</el-button>
        <el-button size="small" type="primary" :loading="saving" @click="saveEdit">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage } from 'element-plus'
import DataTable from '@/components/DataTable.vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const canWrite = computed(() => can('dict.write'))

const TABS = [
  { key: 'user', label: '账号' },
  { key: 'atk', label: 'atk 令牌' },
  { key: 'client', label: '接入账号' },
  { key: 'rtk', label: 'rtk 令牌' },
]
const TYPE_NAMES = { user: '账号', atk: 'atk 令牌', client: '接入账号', rtk: 'rtk 令牌' }
const TAB_HINTS = {
  user: '管理台账号：用于登录管理台，权限 = 所属权限组并集；未分配时按角色回落。',
  atk: '管理台 API Token：只用于调用管理台接口，不能访问 relay 数据面；权限 = 归属账号 ∩ 令牌组。',
  client: '接入账号：relay 数据面的身份主体，也是其 rtk 令牌的权限上限；权限 = 所属权限组并集。',
  rtk: '接入 Token：用于 relay 客户端与管理台 Bearer；权限 = 接入账号 ∩ 令牌组，数据面同样按令牌收窄。',
}
function typeName(t) { return TYPE_NAMES[t] || t }
function tagType(t) { return t === 'user' ? 'primary' : t === 'atk' ? 'warning' : t === 'client' ? 'success' : 'info' }

const tab = ref('user')
const q = ref('')
const items = ref([])
const groups = ref([])
const dlg = ref(false)
const saving = ref(false)
const edit = reactive({ type: '', id: '', name: '', ids: [], ownerGroups: [] })
const availableGroups = computed(() => {
  if (edit.type !== 'atk' && edit.type !== 'rtk') return groups.value
  const own = new Set(edit.ownerGroups || [])
  return groups.value.filter((g) => own.has(g.id) || edit.ids.includes(g.id))
})

function countOf(t) {
  return items.value.filter((x) => x.type === t).length
}

const sortKeys = {
  '对象': 'name',
  '类型': 'type',
  '所属权限组': { key: 'groups', type: 'num', value: (r) => (r.groups || []).length },
  '有效资源': { key: 'resource_count', type: 'num' },
  '设备范围': { key: 'device_count', type: 'num' },
  '状态': 'status'
}

const rows = computed(() => {
  const needle = q.value.trim().toLowerCase()
  return items.value
    .filter((x) => x.type === tab.value)
    .filter((x) => !needle || x.name.toLowerCase().includes(needle) ||
      (x.detail || '').toLowerCase().includes(needle))
    .map((x) => ({ ...x, key: x.type + ':' + x.id }))
})

async function load() {
  const [sub, gs] = await Promise.all([
    Api.get('/api/v1/permission/subjects').catch(() => []),
    Api.get('/api/v1/permission/groups').catch(() => []),
  ])
  items.value = sub || []
  groups.value = gs || []
}

function openEdit(row) {
  edit.type = row.type
  edit.id = row.id
  edit.name = row.name
  edit.ids = (row.groups || []).map((g) => g.id)
  edit.ownerGroups = row.owner_groups || []
  dlg.value = true
}

async function saveEdit() {
  saving.value = true
  try {
    await Api.put('/api/v1/permission/subjects/' + edit.type + '/' + edit.id + '/groups', { ids: edit.ids })
    ElMessage.success('权限组已保存')
    dlg.value = false
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
.w220 { width: 220px; }
.subj-tabs { margin-bottom: 2px; }
.subj-hint { font-size: 12px; margin: 0 0 8px; line-height: 1.5; }
.gtag { margin: 0 4px 2px 0; }
.num { font-family: var(--mono); font-size: 12.5px; }
.sub { font-size: 11px; color: var(--el-text-color-secondary); }
.dlg-tip { font-size: 12px; margin-bottom: 10px; }
.pad { padding: 12px; }
.grp-list { display: flex; flex-direction: column; max-height: 50vh; overflow: auto; }
.grp-item { height: auto; margin: 0; padding: 6px 0; }
.grp-item b { margin-right: 8px; }
.ml6 { margin-left: 6px; }
@media (max-width: 599px) {
  .w220 { width: 140px; }
  .panel-head { flex-wrap: wrap; }
}
</style>
