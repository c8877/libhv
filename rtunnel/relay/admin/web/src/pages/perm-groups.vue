<template>
  <div class="panel pg-panel">
    <div class="panel-head">
      <h2>权限组</h2>
      <span class="muted">功能资源 + 设备资源按组授予；账号、atk、接入账号、rtk 通过加入权限组获得访问权限</span>
      <span class="spacer" />
      <el-button v-if="canWrite" size="small" type="primary" @click="createGroup">+ 新建权限组</el-button>
    </div>

    <div v-if="isPhone" class="pg-mobile-pick">
      <el-select :model-value="form.id || null" size="small" placeholder="选择权限组"
                 class="pg-pick" @change="onMobilePick">
        <el-option v-for="g in groups" :key="g.id" :label="g.name" :value="g.id" />
      </el-select>
      <el-button v-if="canWrite" size="small" type="primary" @click="createGroup">新建</el-button>
    </div>

    <div class="pg-layout">
      <aside class="pg-side">
        <el-input v-model="groupQ" size="small" placeholder="搜索权限组" clearable class="pg-search" />
        <el-menu class="pg-list" :default-active="String(form.id || '')" @select="onGroupSelect">
          <el-menu-item v-for="g in filteredGroups" :key="g.id" :index="String(g.id)" class="pg-item">
            <span class="pg-name">
              <b>{{ g.name }}</b>
              <el-tag v-if="g.super" size="small" type="danger" effect="plain">超级</el-tag>
              <el-tag v-else-if="g.builtin_key" size="small" effect="plain">预置</el-tag>
            </span>
            <span class="pg-meta">
              {{ g.super ? '全部资源' : g.resources.length + ' 资源' }} · {{ g.super || g.all_devices ? '全部' : g.device_count }} 设备 · {{ g.member_count }} 成员
            </span>
          </el-menu-item>
          <div v-if="!filteredGroups.length" class="muted pg-empty">没有匹配的权限组</div>
        </el-menu>
      </aside>

      <section class="pg-main">
        <div class="pg-head">
          <div class="pg-title">
            <span class="muted">{{ form.id ? '编辑权限组' : '新建权限组' }}</span>
            <el-tag v-if="form.super" size="small" type="danger" effect="plain">超级权限：全部功能资源与设备</el-tag>
          </div>
          <div class="pg-actions">
            <el-button v-if="form.id && canWrite && !form.builtin_key" size="small" type="danger" plain
                       @click="removeGroup">删除</el-button>
            <el-button size="small" @click="reload">取消</el-button>
            <el-button v-if="canWrite" size="small" type="primary" :loading="saving" @click="save">保存</el-button>
          </div>
        </div>

        <el-form ref="pgFormRef" :model="form" :rules="pgRules" label-width="62px"
                 class="pg-meta-form" @submit.prevent>
          <el-form-item label="名称" prop="name">
            <el-input v-model="form.name" size="small" maxlength="64" :disabled="!canWrite" placeholder="例如：产线运维" />
          </el-form-item>
          <el-form-item label="允许端口">
            <el-input v-model="form.ports" size="small" :disabled="!canWrite || form.super"
                      placeholder="* 或 22,80-90" />
          </el-form-item>
          <el-form-item label="说明" class="wide">
            <el-input v-model="form.description" size="small" :disabled="!canWrite" placeholder="这个组负责什么" />
          </el-form-item>
        </el-form>

        <div class="pg-flags">
          <el-checkbox v-model="form.all_devices" :disabled="!canWrite || form.super">
            全部设备（含以后新增的设备）
          </el-checkbox>
          <span class="muted">不勾选时，只有下面「设备资源」里选中的设备对该组可见</span>
        </div>

        <el-tabs v-model="tab" class="pg-tabs">
          <el-tab-pane :label="'成员 (' + form.members.length + ')'" name="members">
            <div class="member-hint muted">
              账号 / 接入账号是权限主体；atk 是管理台 API Token，rtk 是接入 Token（同时作用于 relay 数据面）。
              令牌只能在其归属账号的权限内收窄。
            </div>
            <div v-if="!canWrite" class="muted pad">没有修改权限的权限（需要 dict.write）</div>
            <div v-else-if="isPhone" class="m-transfer">
              <div class="m-pane">
                <div class="m-head">已加入 <span class="muted">{{ form.members.length }}</span></div>
                <div class="m-list">
                  <div v-for="k in form.members" :key="k" class="m-item">
                    <span><b>{{ subjectOf(k).name }}</b><em>{{ typeLabel(k) }} · {{ subjectOf(k).detail }}</em></span>
                    <el-button link type="danger" @click="removeMember(k)">移除</el-button>
                  </div>
                  <div v-if="!form.members.length" class="muted pad-sm">还没有成员</div>
                </div>
              </div>
              <el-button class="m-add" @click="memberPicker = true">+ 添加成员</el-button>
            </div>
            <div v-else class="transfer">
              <div class="tr-pane">
                <div class="tr-head">
                  <span>候选对象</span>
                  <el-select v-model="memberType" size="small" class="tr-type">
                    <el-option label="全部类型" value="all" />
                    <el-option label="账号" value="user" />
                    <el-option label="atk 令牌" value="atk" />
                    <el-option label="接入账号" value="client" />
                    <el-option label="rtk 令牌" value="rtk" />
                  </el-select>
                  <el-input v-model="memberQ" size="small" placeholder="搜索" clearable class="tr-search" />
                </div>
                <el-checkbox-group v-model="leftChecked" class="tr-body">
                  <el-checkbox v-for="s in candidates" :key="s.key" :value="s.key" class="tr-item">
                    <span class="tr-name">{{ s.name }}</span>
                    <span class="tr-tag">{{ typeLabel(s.key) }}</span>
                    <span class="tr-detail">{{ s.detail }}</span>
                  </el-checkbox>
                  <div v-if="!candidates.length" class="muted pad-sm">没有可添加的对象</div>
                </el-checkbox-group>
                <div class="tr-foot">{{ candidates.length }} 个可选 · 已加入 {{ form.members.length }}</div>
              </div>
              <div class="tr-ops">
                <el-button size="small" :disabled="!leftChecked.length" @click="addMembers">→</el-button>
                <el-button size="small" :disabled="!rightChecked.length" @click="removeMembers">←</el-button>
              </div>
              <div class="tr-pane">
                <div class="tr-head"><span>已加入</span><span class="muted">{{ form.members.length }}</span></div>
                <el-checkbox-group v-model="rightChecked" class="tr-body">
                  <el-checkbox v-for="k in form.members" :key="k" :value="k" class="tr-item">
                    <span class="tr-name">{{ subjectOf(k).name }}</span>
                    <span class="tr-tag">{{ typeLabel(k) }}</span>
                    <span class="tr-detail">{{ subjectOf(k).detail }}</span>
                  </el-checkbox>
                </el-checkbox-group>
                <div class="tr-foot">不选 = 该对象不属于本组</div>
              </div>
            </div>
          </el-tab-pane>

          <el-tab-pane :label="'功能资源 (' + (form.super ? '全部' : form.resources.length) + ')'" name="resources">
            <div v-if="form.super" class="super-note">超级权限组拥有全部功能资源与全部设备，无需勾选。</div>
            <div class="res-toolbar">
              <span class="muted">已选 {{ form.resources.length }} / {{ resources.length }}</span>
              <span class="spacer" />
              <el-input v-model="resQ" size="small" placeholder="搜索资源名或键" clearable class="res-search" />
              <el-button size="small" :disabled="!canWrite" @click="checkAllResources">全选</el-button>
              <el-button size="small" :disabled="!canWrite" @click="clearResources">清空</el-button>
            </div>
            <div class="res-cats">
              <div v-for="cat in resourceCats" :key="cat.name" class="res-cat">
                <div class="res-cat-head">
                  <b>{{ cat.name }}</b>
                  <span class="muted">{{ cat.checked }} / {{ cat.items.length }}</span>
                  <span class="spacer" />
                  <el-button v-if="canWrite" link size="small" @click="toggleCat(cat)">
                    {{ cat.checked === cat.items.length ? '取消全选' : '全选' }}
                  </el-button>
                </div>
                <div class="res-grid">
                  <label v-for="r in cat.items" :key="r.key" class="res-item"
                         :class="{ on: form.resources.includes(r.key), off: !r.enabled }">
                    <el-checkbox :model-value="form.resources.includes(r.key)" :disabled="!canWrite || !r.enabled"
                                 @change="(v) => toggleResource(r.key, v)" />
                    <span class="res-text">
                      <b>{{ r.name }}</b>
                      <code>{{ r.key }}</code>
                      <em>{{ r.description }}</em>
                    </span>
                  </label>
                </div>
              </div>
            </div>
          </el-tab-pane>

          <el-tab-pane :label="'设备资源 (' + (form.super || form.all_devices ? '全部' : form.devices.length) + ')'" name="devices">
            <div class="dev-tip muted">
              新设备默认不授权；勾选「全部设备」后，该组自动包含以后新增的设备。
            </div>
            <div v-if="form.super" class="dev-all">超级权限组自动包含全部设备</div>
            <div v-else-if="form.all_devices" class="dev-all">已选择全部设备</div>
            <div v-else class="transfer">
              <div class="tr-pane">
                <div class="tr-head">
                  <span>候选设备</span>
                  <el-input v-model="devQ" size="small" placeholder="搜索 devid / 名称" clearable class="tr-search" />
                </div>
                <el-checkbox-group v-model="devLeftChecked" class="tr-body">
                  <el-checkbox v-for="d in devCandidates" :key="d.devid" :value="d.devid" class="tr-item">
                    <span class="tr-name">{{ d.devid }}</span>
                    <span class="tr-detail">{{ d.name || '—' }}</span>
                    <span class="dot" :class="d.online ? 'on' : 'off'" />
                  </el-checkbox>
                  <div v-if="!devCandidates.length" class="muted pad-sm">没有可授权的设备</div>
                </el-checkbox-group>
                <div class="tr-foot">{{ devCandidates.length }} 台可选 · 已选 {{ form.devices.length }}</div>
              </div>
              <div class="tr-ops">
                <el-button size="small" :disabled="!devLeftChecked.length || !canWrite" @click="addDevices">→</el-button>
                <el-button size="small" :disabled="!devRightChecked.length || !canWrite" @click="removeDevices">←</el-button>
              </div>
              <div class="tr-pane">
                <div class="tr-head"><span>已授权</span><span class="muted">{{ form.devices.length }}</span></div>
                <el-checkbox-group v-model="devRightChecked" class="tr-body">
                  <el-checkbox v-for="d in selectedDevices" :key="d.devid" :value="d.devid" class="tr-item">
                    <span class="tr-name">{{ d.devid }}</span>
                    <span class="tr-detail">{{ d.name || '—' }}</span>
                    <span class="dot" :class="d.online ? 'on' : 'off'" />
                  </el-checkbox>
                </el-checkbox-group>
                <div class="tr-foot">未选中的设备对成员不可见</div>
              </div>
            </div>
          </el-tab-pane>
        </el-tabs>

        <div v-if="!form.super && form.resources.includes('device.view') && !form.all_devices && !form.devices.length"
             class="warn">该组有「查看设备」资源，但没有授权任何设备，成员将看不到设备。</div>
      </section>
    </div>

    <!-- 手机端成员选择 -->
    <el-dialog v-model="memberPicker" title="添加成员" :width="isPhone ? '92%' : '520px'">
      <div class="picker">
        <div class="picker-filter">
          <el-select v-model="memberType" size="small">
            <el-option label="全部类型" value="all" />
            <el-option label="账号" value="user" />
            <el-option label="atk 令牌" value="atk" />
            <el-option label="接入账号" value="client" />
            <el-option label="rtk 令牌" value="rtk" />
          </el-select>
          <el-input v-model="memberQ" size="small" placeholder="搜索" clearable />
        </div>
        <el-checkbox-group v-model="mobileChecked" class="picker-list">
          <el-checkbox v-for="s in candidates" :key="s.key" :value="s.key" class="tr-item">
            <span class="tr-name">{{ s.name }}</span>
            <span class="tr-tag">{{ typeLabel(s.key) }}</span>
            <span class="tr-detail">{{ s.detail }}</span>
          </el-checkbox>
        </el-checkbox-group>
      </div>
      <template #footer>
        <el-button size="small" @click="memberPicker = false">取消</el-button>
        <el-button size="small" type="primary" @click="addMembersMobile">添加</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const canWrite = computed(() => can('dict.write'))

const groups = ref([])
const resources = ref([])
const devices = ref([])
const subjects = ref([])
const tab = ref('members')
const groupQ = ref('')
const saving = ref(false)
const memberPicker = ref(false)

const form = reactive({
  id: 0,
  name: '',
  description: '',
  super: false,
  builtin_key: '',
  all_devices: false,
  ports: '*',
  resources: [],
  devices: [],
  members: [],
})
const pgFormRef = ref(null)
const pgRules = {
  name: [{ required: true, message: '请填写权限组名称', trigger: 'blur' }],
}

const leftChecked = ref([])
const rightChecked = ref([])
const devLeftChecked = ref([])
const devRightChecked = ref([])
const mobileChecked = ref([])
const memberType = ref('all')
const memberQ = ref('')
const devQ = ref('')
const resQ = ref('')

const TYPE_NAMES = { user: '账号', atk: 'atk', client: '接入账号', rtk: 'rtk' }
function typeLabel(key) {
  return TYPE_NAMES[String(key).split(':')[0]] || ''
}
function keyOf(m) {
  return m.type + ':' + m.id
}

const subjectMap = computed(() => {
  const m = new Map()
  subjects.value.forEach((s) => m.set(s.type + ':' + s.id, s))
  return m
})
function subjectOf(key) {
  return subjectMap.value.get(key) || { name: key, detail: '', type: String(key).split(':')[0] }
}

const filteredGroups = computed(() => {
  const q = groupQ.value.trim().toLowerCase()
  return q ? groups.value.filter((g) => g.name.toLowerCase().includes(q)) : groups.value
})

const selectedKeySet = computed(() => new Set(form.members))
const candidates = computed(() => {
  const q = memberQ.value.trim().toLowerCase()
  const ty = memberType.value
  return subjects.value
    .map((s) => ({ ...s, key: s.type + ':' + s.id }))
    .filter((s) => !selectedKeySet.value.has(s.key))
    .filter((s) => ty === 'all' || s.type === ty)
    .filter((s) => !q || s.name.toLowerCase().includes(q) || (s.detail || '').toLowerCase().includes(q))
})

const devMap = computed(() => {
  const m = new Map()
  devices.value.forEach((d) => m.set(d.devid, d))
  return m
})
const devCandidates = computed(() => {
  const q = devQ.value.trim().toLowerCase()
  const selected = new Set(form.devices)
  return devices.value
    .filter((d) => !selected.has(d.devid))
    .filter((d) => !q || d.devid.toLowerCase().includes(q) || (d.name || '').toLowerCase().includes(q))
})
const selectedDevices = computed(() =>
  form.devices.map((id) => devMap.value.get(id) || { devid: id, name: '', online: false })
)

const resourceCats = computed(() => {
  const q = resQ.value.trim().toLowerCase()
  const map = new Map()
  resources.value.forEach((r) => {
    if (q && !(r.name.toLowerCase().includes(q) || r.key.toLowerCase().includes(q) ||
               (r.description || '').toLowerCase().includes(q))) return
    if (!map.has(r.category)) map.set(r.category, [])
    map.get(r.category).push(r)
  })
  const out = []
  for (const [name, items] of map) {
    out.push({ name, items, checked: items.filter((i) => form.resources.includes(i.key)).length })
  }
  return out
})

function resetForm() {
  Object.assign(form, {
    id: 0, name: '', description: '', super: false, builtin_key: '',
    all_devices: false, ports: '*', resources: [], devices: [], members: [],
  })
  leftChecked.value = []
  rightChecked.value = []
  devLeftChecked.value = []
  devRightChecked.value = []
  mobileChecked.value = []
  tab.value = 'members'
}

async function loadGroups() {
  groups.value = (await Api.get('/api/v1/permission/groups').catch(() => [])) || []
}

async function loadRefs() {
  const [res, dev, sub] = await Promise.all([
    Api.get('/api/v1/permission/resources').catch(() => []),
    Api.get('/api/v1/permission/dicts?kind=devices&page=1&page_size=200').catch(() => null),
    Api.get('/api/v1/permission/subjects').catch(() => []),
  ])
  resources.value = res || []
  devices.value = (dev && dev.items) || []
  subjects.value = sub || []
}

async function reload() {
  await Promise.all([loadGroups(), loadRefs()])
  if (form.id && groups.value.some((g) => g.id === form.id)) await selectGroup({ id: form.id })
  else if (groups.value.length) await selectGroup(groups.value[0])
  else resetForm()
}

async function selectGroup(g) {
  const d = await Api.get('/api/v1/permission/groups/' + g.id).catch(() => null)
  if (!d) return
  Object.assign(form, {
    id: d.id,
    name: d.name,
    description: d.description || '',
    super: !!d.super,
    builtin_key: d.builtin_key || '',
    all_devices: !!d.all_devices,
    ports: d.ports || '*',
    resources: d.resources || [],
    devices: d.devices || [],
    members: (d.members || []).map(keyOf),
  })
  leftChecked.value = []
  rightChecked.value = []
  devLeftChecked.value = []
  devRightChecked.value = []
}

function createGroup() {
  resetForm()
  form.name = ''
  tab.value = 'members'
}

function onMobilePick(id) {
  const g = groups.value.find((x) => x.id === id)
  if (g) selectGroup(g)
}

function onGroupSelect(id) {
  const g = groups.value.find((x) => String(x.id) === String(id))
  if (g) selectGroup(g)
}

async function save() {
  const ok = await pgFormRef.value.validate().catch(() => false)
  if (!ok) return
  saving.value = true
  try {
    const body = {
      name: form.name.trim(),
      description: form.description.trim(),
      all_devices: !!form.all_devices,
      ports: form.ports.trim() || '*',
      resources: form.resources.slice(),
      devices: form.all_devices ? [] : form.devices.slice(),
      members: form.members.map((k) => {
        const i = k.indexOf(':')
        return { type: k.slice(0, i), id: k.slice(i + 1) }
      }),
    }
    const r = form.id
      ? await Api.put('/api/v1/permission/groups/' + form.id, body)
      : await Api.post('/api/v1/permission/groups', body)
    ElMessage.success('权限组已保存')
    const id = (r && r.id) || form.id
    await loadGroups()
    if (id) await selectGroup({ id })
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function removeGroup() {
  try {
    await ElMessageBox.confirm('删除权限组「' + form.name + '」？成员会立即失去该组资源。', '删除权限组', {
      type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消',
    })
  } catch (e) {
    return
  }
  try {
    await Api.del('/api/v1/permission/groups/' + form.id)
    ElMessage.success('已删除')
    form.id = 0
    await reload()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

function addMembers() {
  const add = leftChecked.value.filter((k) => !form.members.includes(k))
  form.members.push(...add)
  leftChecked.value = []
}
function removeMembers() {
  const drop = new Set(rightChecked.value)
  form.members = form.members.filter((k) => !drop.has(k))
  rightChecked.value = []
}
function removeMember(k) {
  form.members = form.members.filter((x) => x !== k)
}
function addMembersMobile() {
  const add = mobileChecked.value.filter((k) => !form.members.includes(k))
  form.members.push(...add)
  mobileChecked.value = []
  memberPicker.value = false
}

function addDevices() {
  const add = devLeftChecked.value.filter((k) => !form.devices.includes(k))
  form.devices.push(...add)
  devLeftChecked.value = []
}
function removeDevices() {
  const drop = new Set(devRightChecked.value)
  form.devices = form.devices.filter((k) => !drop.has(k))
  devRightChecked.value = []
}

function toggleResource(key, on) {
  if (on) {
    if (!form.resources.includes(key)) form.resources.push(key)
  } else {
    form.resources = form.resources.filter((k) => k !== key)
  }
}
function checkAllResources() {
  form.resources = resources.value.filter((r) => r.enabled).map((r) => r.key)
}
function clearResources() {
  form.resources = []
}
function toggleCat(cat) {
  const keys = cat.items.filter((r) => r.enabled).map((r) => r.key)
  if (cat.checked === cat.items.length) {
    const drop = new Set(keys)
    form.resources = form.resources.filter((k) => !drop.has(k))
  } else {
    keys.forEach((k) => { if (!form.resources.includes(k)) form.resources.push(k) })
  }
}

onMounted(reload)
</script>

<style scoped>
.pg-panel { padding-bottom: 12px; }
.pg-layout { display: grid; grid-template-columns: 240px minmax(0, 1fr); gap: 14px; align-items: start; }
.pg-side { border-right: 1px solid var(--el-border-color-lighter); padding-right: 12px; }
.pg-search { margin-bottom: 8px; }
.pg-list { max-height: 560px; overflow: auto; border-right: 0; background: transparent; }
.pg-list :deep(.el-menu-item) {
  display: flex; flex-direction: column; align-items: flex-start; gap: 2px;
  height: auto; line-height: 1.4; padding: 7px 9px; border-radius: 6px;
}
.pg-list :deep(.el-menu-item:hover) { background: var(--el-fill-color-light); }
.pg-list :deep(.el-menu-item.is-active) { background: var(--el-color-primary-light-9); }
.pg-list :deep(.el-menu-item.is-active) .pg-name b { color: var(--el-color-primary); }
.pg-name { display: flex; align-items: center; gap: 6px; }
.pg-name b { font-size: 13px; font-weight: 600; }
.pg-meta { font-size: 11px; color: var(--el-text-color-secondary); }
.pg-empty { padding: 12px 8px; font-size: 12px; }
.pg-main { min-width: 0; }
.pg-head { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
.pg-title { display: flex; align-items: center; gap: 8px; min-width: 0; }
.pg-actions { margin-left: auto; display: flex; gap: 6px; }
.pg-meta-form { display: grid; grid-template-columns: 1fr 160px; gap: 8px 18px; margin-bottom: 6px; }
.pg-meta-form :deep(.el-form-item) { margin-bottom: 0; min-width: 0; }
.pg-meta-form .wide { grid-column: 1 / -1; }
.pg-flags { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; margin: 2px 0 6px; font-size: 12px; }
.pg-tabs { margin-top: 2px; }

.transfer { display: grid; grid-template-columns: minmax(0, 1fr) 36px minmax(0, 1fr); gap: 8px; align-items: stretch; }
.tr-pane {
  display: flex; flex-direction: column; min-width: 0;
  border: 1px solid var(--el-border-color-lighter); border-radius: 8px; overflow: hidden;
  background: var(--el-fill-color-blank);
}
.tr-head { display: flex; align-items: center; gap: 6px; padding: 7px 9px; border-bottom: 1px solid var(--el-border-color-lighter); font-size: 12px; }
.tr-type { width: 92px; }
.tr-search { flex: 1; min-width: 80px; }
.tr-body { flex: 1; max-height: 320px; overflow: auto; padding: 3px 0; width: 100%; }
.tr-item { display: flex; align-items: center; gap: 6px; width: 100%; height: auto; margin: 0; padding: 5px 9px; }
.tr-item:hover { background: var(--el-fill-color-light); }
.tr-item :deep(.el-checkbox__label) { display: flex; align-items: center; gap: 6px; min-width: 0; width: 100%; }
.tr-name { font-size: 12.5px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.tr-tag { flex: none; font-size: 10px; padding: 0 5px; border-radius: 3px; background: var(--el-fill-color); color: var(--el-text-color-secondary); }
.tr-detail { min-width: 0; font-size: 11px; color: var(--el-text-color-secondary); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.tr-ops { display: flex; flex-direction: column; justify-content: center; gap: 6px; }
.tr-ops .el-button { margin: 0; }
.tr-foot { padding: 5px 9px; border-top: 1px solid var(--el-border-color-lighter); font-size: 11px; color: var(--el-text-color-secondary); }

.res-toolbar { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.res-toolbar .spacer { flex: 1; }
.res-search { width: 210px; }
.res-cats { max-height: 460px; overflow: auto; padding-right: 4px; }
.res-cat { margin-bottom: 10px; }
.res-cat-head { display: flex; align-items: center; gap: 8px; padding: 3px 0; border-bottom: 1px solid var(--el-border-color-lighter); }
.res-cat-head .spacer { flex: 1; }
.res-cat-head b { font-size: 12.5px; }
.res-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 2px 16px; margin-top: 4px; }
.res-item { display: flex; align-items: flex-start; gap: 6px; padding: 4px 6px; border-radius: 5px; cursor: pointer; }
.res-item:hover { background: var(--el-fill-color-light); }
.res-item.on { background: var(--el-color-primary-light-9); }
.res-item.off { opacity: 0.45; }
.res-text { display: flex; flex-direction: column; min-width: 0; line-height: 1.3; }
.res-text b { font-size: 12.5px; font-weight: 600; }
.res-text code { font-size: 10.5px; color: var(--el-text-color-secondary); }
.res-text em { font-size: 11px; color: var(--el-text-color-secondary); font-style: normal; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.dev-tip { font-size: 12px; margin-bottom: 8px; }
.dev-all { padding: 18px; text-align: center; color: var(--el-color-primary); background: var(--el-color-primary-light-9); border-radius: 8px; }
.dot { flex: none; width: 6px; height: 6px; border-radius: 50%; margin-left: auto; }
.dot.on { background: var(--el-color-success); }
.dot.off { background: var(--el-color-info-light-5); }
.super-note { margin: 4px 0 10px; padding: 8px 10px; border-radius: 6px; background: var(--el-color-primary-light-9); color: var(--el-color-primary); font-size: 12px; }
.warn { margin-top: 10px; padding: 8px 10px; border-radius: 6px; background: var(--el-color-warning-light-9); color: var(--el-color-warning-dark-2); font-size: 12px; }
.member-hint { font-size: 12px; margin: 0 0 10px; line-height: 1.5; }
.pad { padding: 16px; }
.pad-sm { padding: 10px; font-size: 12px; }
.picker-filter { display: flex; gap: 8px; margin-bottom: 8px; }
.picker-list { max-height: 52vh; overflow: auto; display: flex; flex-direction: column; }
.m-transfer { display: flex; flex-direction: column; gap: 8px; }
.m-pane { border: 1px solid var(--el-border-color-lighter); border-radius: 8px; overflow: hidden; }
.m-head { padding: 7px 10px; font-size: 12px; border-bottom: 1px solid var(--el-border-color-lighter); }
.m-list { max-height: 300px; overflow: auto; }
.m-item { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 7px 10px; border-bottom: 1px solid var(--el-border-color-lighter); }
.m-item:last-child { border-bottom: 0; }
.m-item span { display: flex; flex-direction: column; min-width: 0; }
.m-item b { font-size: 12.5px; }
.m-item em { font-size: 11px; color: var(--el-text-color-secondary); font-style: normal; }
.m-add { width: 100%; }

.pg-mobile-pick { display: none; align-items: center; gap: 8px; margin-bottom: 10px; }
.pg-pick { flex: 1; min-width: 0; }

@media (max-width: 899px) {
  .pg-panel .panel-head { flex-wrap: wrap; }
  .pg-panel .panel-head h2 { flex: none; white-space: nowrap; }
  .pg-panel .panel-head > .muted { flex: 1 0 100%; order: 3; margin-top: 2px; }
  .pg-panel .panel-head .spacer { display: none; }
  .pg-panel .panel-head > .el-button { margin-left: auto; }
  .pg-layout { grid-template-columns: 1fr; }
  .pg-side { display: none; }
  .pg-mobile-pick { display: flex; }
  .pg-meta-form { grid-template-columns: 1fr; }
  .pg-meta-form .wide { grid-column: auto; }
  .transfer { grid-template-columns: 1fr; }
  .tr-ops { flex-direction: row; justify-content: center; }
  .res-grid { grid-template-columns: 1fr; }
  .pg-head { flex-wrap: wrap; }
  .pg-actions { width: 100%; justify-content: flex-end; }
  .res-toolbar { flex-wrap: wrap; }
  .res-search { width: 100%; }
}
@media (max-width: 599px) {
  .pg-panel .panel-head > .el-button { display: none; }
}
</style>
