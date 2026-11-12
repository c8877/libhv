<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>权限管理</h2>
        <span class="muted">客户端分组 → 设备分组，按 ACL 授权能力</span>
        <span class="spacer" />
        <el-button @click="openEffective">有效权限</el-button>
        <el-button v-if="canManage()" @click="openLegacy">旧 ACL</el-button>
        <el-button v-if="canManage()" @click="openDict">字典管理</el-button>
        <el-button v-if="canManage()" type="primary" @click="newGroup">+ 新建分组</el-button>
      </div>

      <el-tabs v-model="tab" @tab-change="onTab">
        <el-tab-pane name="client">
          <template #label>客户端分组 <span class="cnt">{{ overview.client_groups.length }}</span></template>
        </el-tab-pane>
        <el-tab-pane name="agent">
          <template #label>设备分组 <span class="cnt">{{ overview.agent_groups.length }}</span></template>
        </el-tab-pane>
      </el-tabs>

      <el-table :data="groups" v-loading="loading" row-key="id" :row-class-name="rowClass" @row-click="openRow">
        <template v-if="isPhone">
          <el-table-column label="分组">
            <template #default="{ row }">
              <div><b>{{ row.name }}</b></div>
              <div class="muted">{{ countText(row) }}</div>
              <div class="muted caps">{{ capText(row.caps) }}</div>
            </template>
          </el-table-column>
        </template>
        <template v-else>
          <el-table-column label="名称" min-width="180">
            <template #default="{ row }">
              <span class="chev">{{ isOpen(row) ? '▾' : '▸' }}</span>
              <b>{{ row.name }}</b>
              <div v-if="row.description" class="muted">{{ row.description }}</div>
            </template>
          </el-table-column>
          <el-table-column :label="tab === 'client' ? '用户数' : '设备数'" width="100" align="right">
            <template #default="{ row }">{{ tab === 'client' ? row.user_count || 0 : row.device_count || 0 }}</template>
          </el-table-column>
          <el-table-column :label="tab === 'client' ? '设备组' : '被引用'" width="100" align="right">
            <template #default="{ row }">{{ tab === 'client' ? row.acl_count || 0 : row.ref_count || 0 }}</template>
          </el-table-column>
          <el-table-column label="能力摘要" min-width="240">
            <template #default="{ row }"><span class="muted caps">{{ capText(row.caps) }}</span></template>
          </el-table-column>
          <el-table-column label="更新" min-width="100">
            <template #default="{ row }"><span class="muted">{{ ago(row.updated_ms) }}前</span></template>
          </el-table-column>
        </template>
        <el-table-column :width="isPhone ? 70 : 90" label="操作">
          <template #default="{ row }">
            <el-button v-if="canManage()" link type="danger" @click.stop="removeGroup(row)">删除</el-button>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <template #empty><el-empty description="暂无分组" :image-size="70" /></template>
      </el-table>
    </div>

    <!-- 分组编辑 -->
    <div v-if="editing" ref="editorEl" class="panel editor">
      <div class="panel-head">
        <h2>{{ editing.id ? '编辑' : '新建' }}{{ editing.kind === 'client' ? '客户端分组' : '设备分组' }}</h2>
        <span class="spacer" />
        <el-button @click="closeEditor">关闭</el-button>
      </div>

      <div class="form-grid">
        <label>名称<el-input v-model="editing.name" maxlength="64" placeholder="分组名称" /></label>
        <label>描述<el-input v-model="editing.description" maxlength="256" placeholder="可选" /></label>
      </div>

      <template v-if="editing.kind === 'client'">
        <h3>客户端用户</h3>
        <el-select v-model="editing.users" multiple filterable allow-create default-first-option
                   placeholder="选择或输入用户名" class="w100">
          <el-option v-for="a in accounts" :key="a.username" :label="a.username" :value="a.username" />
        </el-select>

        <h3>ACL 配置</h3>
        <div v-if="!editing.acl.length" class="muted empty-acl">还没有 ACL，点下方“添加 ACL”授权设备分组。</div>

        <el-table v-if="!isPhone && editing.acl.length" :data="editing.acl" size="small" class="acl-table">
          <el-table-column label="设备分组" width="150">
            <template #default="{ row }">
              <el-select v-model="row.agent_group_id" size="small">
                <el-option v-for="g in overview.agent_groups" :key="g.id" :label="g.name" :value="g.id" />
              </el-select>
            </template>
          </el-table-column>
          <el-table-column label="TCP 端口" width="150">
            <template #default="{ row }"><el-input v-model="row.tcp_ports" size="small" placeholder="22,80,*" /></template>
          </el-table-column>
          <el-table-column label="Shell" width="62" align="center">
            <template #default="{ row }"><el-checkbox v-model="row.shell" /></template>
          </el-table-column>
          <el-table-column label="TTY" width="130">
            <template #default="{ row }"><el-input v-model="row.tty" size="small" placeholder="ttyS0,*" /></template>
          </el-table-column>
          <el-table-column label="文件" width="88">
            <template #default="{ row }">
              <el-select v-model="row.file" size="small">
                <el-option label="none" value="none" />
                <el-option label="ro" value="ro" />
                <el-option label="rw" value="rw" />
              </el-select>
            </template>
          </el-table-column>
          <el-table-column label="SYSINFO" width="76" align="center">
            <template #default="{ row }"><el-checkbox v-model="row.sysinfo" /></template>
          </el-table-column>
          <el-table-column label="指标" width="62" align="center">
            <template #default="{ row }"><el-checkbox v-model="row.metrics" /></template>
          </el-table-column>
          <el-table-column label="配置" width="62" align="center">
            <template #default="{ row }"><el-checkbox v-model="row.config" /></template>
          </el-table-column>
          <el-table-column label="Web" width="56" align="center">
            <template #default="{ row }"><el-checkbox v-model="row.web" /></template>
          </el-table-column>
          <el-table-column width="64">
            <template #default="{ $index }">
              <el-button link type="danger" @click="editing.acl.splice($index, 1)">移除</el-button>
            </template>
          </el-table-column>
        </el-table>

        <div v-else-if="isPhone && editing.acl.length" class="acl-cards">
          <div v-for="(a, i) in editing.acl" :key="i" class="acl-card">
            <div class="acl-card-head">
              <el-select v-model="a.agent_group_id" size="small" class="grow">
                <el-option v-for="g in overview.agent_groups" :key="g.id" :label="g.name" :value="g.id" />
              </el-select>
              <el-button link type="danger" @click="editing.acl.splice(i, 1)">移除</el-button>
            </div>
            <div class="acl-card-grid">
              <label>TCP 端口<el-input v-model="a.tcp_ports" size="small" placeholder="22,80,*" /></label>
              <label>TTY<el-input v-model="a.tty" size="small" placeholder="ttyS0,*" /></label>
              <label>文件
                <el-select v-model="a.file" size="small">
                  <el-option label="none" value="none" />
                  <el-option label="ro" value="ro" />
                  <el-option label="rw" value="rw" />
                </el-select>
              </label>
            </div>
            <div class="checks">
              <el-checkbox v-model="a.shell">Shell</el-checkbox>
              <el-checkbox v-model="a.sysinfo">SYSINFO</el-checkbox>
              <el-checkbox v-model="a.metrics">指标</el-checkbox>
              <el-checkbox v-model="a.config">配置</el-checkbox>
              <el-checkbox v-model="a.web">Web</el-checkbox>
            </div>
          </div>
        </div>

        <el-button class="mt10" @click="addAcl">添加 ACL</el-button>
      </template>

      <template v-else>
        <h3>设备成员</h3>
        <div class="chips">
          <el-tag v-for="d in editing.devices" :key="d" closable @close="removeDevice(d)">{{ d }}</el-tag>
          <span v-if="!editing.devices.length" class="muted">还没有设备</span>
        </div>
        <el-button class="mt10" @click="openDevicePicker">+ 添加设备</el-button>

        <h3>能力上限</h3>
        <div class="form-grid caps-grid">
          <label>TCP 端口<el-input v-model="editing.caps.tcp_ports" placeholder="22,80,*" /></label>
          <label>TTY<el-input v-model="editing.caps.tty" placeholder="ttyS0,*" /></label>
          <label>文件
            <el-select v-model="editing.caps.file">
              <el-option label="none" value="none" />
              <el-option label="ro" value="ro" />
              <el-option label="rw" value="rw" />
            </el-select>
          </label>
        </div>
        <div class="checks">
          <el-checkbox v-model="editing.caps.shell">Shell</el-checkbox>
          <el-checkbox v-model="editing.caps.sysinfo">SYSINFO</el-checkbox>
          <el-checkbox v-model="editing.caps.metrics">指标</el-checkbox>
          <el-checkbox v-model="editing.caps.config">修改配置</el-checkbox>
          <el-checkbox v-model="editing.caps.web">Web</el-checkbox>
        </div>

        <template v-if="editing.referenced_by && editing.referenced_by.length">
          <h3>被客户端分组引用</h3>
          <div class="chips">
            <el-tag v-for="r in editing.referenced_by" :key="r" size="small">{{ r }}</el-tag>
          </div>
        </template>
      </template>

      <div class="editor-foot">
        <span v-if="!canManage()" class="muted ro-hint">只读：当前角色不能修改权限</span>
        <el-button @click="closeEditor">取消</el-button>
        <el-button v-if="canManage()" type="primary" :loading="saving" @click="save">保存</el-button>
      </div>
    </div>

    <!-- 添加设备 -->
    <el-dialog v-model="devDlg" title="添加设备" width="min(92vw, 560px)">
      <el-input v-model="devQ" placeholder="搜索 devid" clearable class="mb10">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <div class="cand-list">
        <el-checkbox-group v-model="devPick">
          <el-checkbox v-for="d in filteredCandidates" :key="d.devid" :value="d.devid"
                       :disabled="editing && editing.devices.includes(d.devid)" class="cand-item">
            <span class="mono">{{ d.devid }}</span>
            <el-tag :type="d.online ? 'success' : 'info'" size="small">{{ d.online ? '在线' : '登记' }}</el-tag>
          </el-checkbox>
        </el-checkbox-group>
        <el-empty v-if="!filteredCandidates.length" description="没有可添加的设备" :image-size="60" />
      </div>
      <template #footer>
        <el-button @click="devDlg = false">取消</el-button>
        <el-button type="primary" @click="confirmDevices">添加 {{ devPick.length }} 台</el-button>
      </template>
    </el-dialog>

    <!-- 字典 -->
    <el-dialog v-model="dictDlg" title="字典管理" width="min(94vw, 760px)">
      <el-tabs v-model="dictTab" @tab-change="loadDict">
        <el-tab-pane label="端口字典" name="ports" />
        <el-tab-pane label="TTY 字典" name="ttys" />
      </el-tabs>
      <div class="dict-add">
        <el-input v-model="dictNew.value" placeholder="端口/范围，如 22 或 8000-8100" class="grow" />
        <el-input v-model="dictNew.name" placeholder="名称（可选）" class="w160" />
        <el-button type="primary" @click="addDict">新增</el-button>
        <el-button @click="batchDlg = true">批量新增</el-button>
        <el-input v-model="dictQ" placeholder="搜索" clearable class="w160">
          <template #prefix><el-icon><Search /></el-icon></template>
        </el-input>
      </div>
      <el-table :data="filteredDict" size="small" v-loading="dictLoading">
        <el-table-column :label="dictTab === 'ports' ? '端口/范围' : 'TTY 路径/模式'" min-width="150">
          <template #default="{ row }">
            <code>{{ row.value }}</code>
            <div v-if="isPhone && row.name" class="muted">{{ row.name }}</div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="name" label="名称" min-width="120" />
        <el-table-column v-if="!isPhone" label="引用" width="70" align="right">
          <template #default="{ row }">{{ row.refs || 0 }}</template>
        </el-table-column>
        <el-table-column label="状态" width="80">
          <template #default="{ row }">
            <el-tag :type="row.enabled ? 'success' : 'info'" size="small">{{ row.enabled ? '启用' : '停用' }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="110">
          <template #default="{ row }">
            <el-button link type="primary" @click="openDictEdit(row)">编辑</el-button>
            <el-button link type="danger" @click="removeDict(row)">删除</el-button>
          </template>
        </el-table-column>
        <template #empty><el-empty description="暂无字典项" :image-size="60" /></template>
      </el-table>
    </el-dialog>

    <el-dialog v-model="dictEditDlg" title="编辑字典项" width="min(92vw, 420px)">
      <el-form label-width="72px">
        <el-form-item label="值"><el-input v-model="dictEdit.value" disabled /></el-form-item>
        <el-form-item label="名称"><el-input v-model="dictEdit.name" maxlength="64" /></el-form-item>
        <el-form-item label="描述"><el-input v-model="dictEdit.description" maxlength="128" /></el-form-item>
        <el-form-item label="状态"><el-switch v-model="dictEdit.enabled" /></el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dictEditDlg = false">取消</el-button>
        <el-button type="primary" @click="saveDictEdit">保存</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="batchDlg" title="批量新增字典项" width="min(92vw, 520px)">
      <el-input v-model="batchText" type="textarea" :rows="5" class="mono"
                :placeholder="dictTab === 'ports' ? '22,80,443,8000-8100' : 'ttyS0,ttyS1,ttyUSB*,ttyACM*'" />
      <div class="mt10"><el-input v-model="batchPrefix" placeholder="名称前缀（可选）" /></div>
      <div class="muted mt6">逗号或换行分隔，重复项会跳过。</div>
      <template #footer>
        <el-button @click="batchDlg = false">取消</el-button>
        <el-button type="primary" @click="batchAdd">批量添加</el-button>
      </template>
    </el-dialog>

    <!-- 有效权限 -->
    <el-dialog v-model="effDlg" title="有效权限" width="min(92vw, 620px)">
      <div class="eff-row">
        <el-input v-model="effClient" placeholder="客户端用户名" @keyup.enter="loadEffective" />
        <el-button type="primary" :loading="effLoading" @click="loadEffective">查看</el-button>
      </div>
      <template v-if="eff">
        <el-descriptions :column="1" size="small" border class="mt10">
          <el-descriptions-item label="用户">{{ eff.client }}</el-descriptions-item>
          <el-descriptions-item label="所属分组">{{ (eff.groups || []).join('、') || '—' }}</el-descriptions-item>
          <el-descriptions-item label="能力">{{ capText(eff.caps) }}</el-descriptions-item>
        </el-descriptions>
        <div class="mt10">
          <div class="muted mb6">可访问设备（{{ (eff.devices || []).length }}）</div>
          <div class="chips">
            <el-tag v-for="d in eff.devices" :key="d" size="small">{{ d }}</el-tag>
            <span v-if="!(eff.devices || []).length" class="muted">无</span>
          </div>
        </div>
      </template>
    </el-dialog>

    <!-- 旧 ACL -->
    <el-dialog v-model="legacyDlg" title="旧 ACL" width="min(94vw, 760px)">
      <div class="muted mb10">
        旧格式每行 <code>user=devid,devid</code>，只能表达能力全开的授权；导入会创建同名分组。
      </div>
      <el-input v-model="legacyText" type="textarea" :rows="14" class="mono" />
      <template #footer>
        <el-button @click="exportLegacy">从当前配置生成</el-button>
        <el-button @click="downloadLegacy">下载</el-button>
        <span class="spacer" />
        <el-button @click="legacyDlg = false">关闭</el-button>
        <el-button v-if="isAdmin" type="primary" @click="importLegacy">导入</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, reactive, ref } from 'vue'
import { Api } from '~/shared/api.js'
import { canManage, roleOf } from '~/shared/store.js'
import { ago } from '~/shared/utils.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const isAdmin = computed(() => roleOf() === 'admin')

const tab = ref('client')
const overview = ref({ client_groups: [], agent_groups: [] })
const loading = ref(false)
const editing = ref(null)
const editorEl = ref(null)
const saving = ref(false)
const accounts = ref([])
const groups = computed(() => (tab.value === 'client' ? overview.value.client_groups : overview.value.agent_groups))

function truthy(v) { return v === true || v === '1' || v === 1 }
function capsOf(c) {
  c = c || {}
  return {
    tcp_ports: c.tcp_ports || '',
    tty: c.tty || '',
    file: c.file || 'none',
    shell: truthy(c.shell),
    sysinfo: truthy(c.sysinfo),
    metrics: truthy(c.metrics),
    config: truthy(c.config),
    web: truthy(c.web),
  }
}
function capText(c) {
  c = c || {}
  const bits = []
  if (truthy(c.shell)) bits.push('Shell')
  if (c.tcp_ports) bits.push('TCP ' + c.tcp_ports)
  if (c.tty) bits.push('TTY ' + c.tty)
  if (c.file && c.file !== 'none') bits.push('文件 ' + c.file)
  if (truthy(c.sysinfo)) bits.push('SYSINFO')
  if (truthy(c.metrics)) bits.push('指标')
  if (truthy(c.config)) bits.push('配置')
  if (truthy(c.web)) bits.push('Web')
  return bits.join(' · ') || '无'
}
function countText(row) {
  return tab.value === 'client'
    ? (row.user_count || 0) + ' 用户 · ' + (row.acl_count || 0) + ' 设备组'
    : (row.device_count || 0) + ' 设备 · 被 ' + (row.ref_count || 0) + ' 组引用'
}
function isOpen(row) {
  return editing.value && editing.value.kind === tab.value && editing.value.id === row.id
}
function rowClass({ row }) {
  return isOpen(row) ? 'perm-open' : ''
}

async function load(silent) {
  if (!silent) loading.value = true
  try {
    overview.value = await Api.get('/api/v1/permission/overview')
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function loadAccounts() {
  try { accounts.value = await Api.get('/api/v1/client-accounts') } catch (e) { accounts.value = [] }
}

function onTab() {
  editing.value = null
}

async function openRow(row) {
  if (editing.value && editing.value.kind === tab.value && editing.value.id === row.id) {
    closeEditor()
    return
  }
  try {
    const base = tab.value === 'client' ? 'client-groups' : 'agent-groups'
    const d = await Api.get('/api/v1/permission/' + base + '/' + row.id)
    if (tab.value === 'client') {
      editing.value = {
        kind: 'client',
        id: d.id,
        name: d.name || '',
        description: d.description || '',
        users: Array.isArray(d.users) ? d.users.slice() : [],
        acl: (d.acl || []).map((a) => ({
          agent_group_id: a.agent_group_id,
          tcp_ports: a.tcp_ports || '',
          tty: a.tty || '',
          file: a.file || 'none',
          shell: !!a.shell,
          sysinfo: !!a.sysinfo,
          metrics: !!a.metrics,
          config: !!a.config,
          web: !!a.web,
        })),
      }
    } else {
      editing.value = {
        kind: 'agent',
        id: d.id,
        name: d.name || '',
        description: d.description || '',
        devices: Array.isArray(d.devices) ? d.devices.slice() : [],
        caps: capsOf(d.caps),
        referenced_by: d.referenced_by || [],
      }
      await loadCandidates(d.id)
    }
    scrollEditor()
  } catch (e) {
    ElMessage.error('加载分组失败：' + e.message)
  }
}

function newGroup() {
  if (tab.value === 'client') {
    editing.value = { kind: 'client', id: 0, name: '', description: '', users: [], acl: [] }
  } else {
    editing.value = { kind: 'agent', id: 0, name: '', description: '', devices: [], caps: capsOf({}), referenced_by: [] }
    loadCandidates(0)
  }
  scrollEditor()
}

function closeEditor() {
  editing.value = null
}

function scrollEditor() {
  nextTick(() => {
    if (editorEl.value) editorEl.value.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  })
}

function addAcl() {
  const used = new Set(editing.value.acl.map((a) => a.agent_group_id))
  const avail = overview.value.agent_groups.find((g) => !used.has(g.id))
  if (!avail) {
    ElMessage.warning('没有可添加的设备分组')
    return
  }
  editing.value.acl.push({
    agent_group_id: avail.id, tcp_ports: '', tty: '', file: 'none',
    shell: false, sysinfo: false, metrics: false, config: false, web: false,
  })
}

async function save() {
  const f = editing.value
  const name = String(f.name || '').trim()
  if (!name) {
    ElMessage.warning('名称必填')
    return
  }
  if (f.kind === 'client') {
    const ids = f.acl.map((a) => a.agent_group_id)
    if (new Set(ids).size !== ids.length) {
      ElMessage.warning('同一个设备分组只能配置一条 ACL')
      return
    }
    if (f.acl.some((a) => !a.agent_group_id)) {
      ElMessage.warning('请选择 ACL 的设备分组')
      return
    }
  } else if (new Set(f.devices).size !== f.devices.length) {
    ElMessage.warning('设备成员重复')
    return
  }

  saving.value = true
  try {
    let body
    let base
    if (f.kind === 'client') {
      base = 'client-groups'
      body = {
        name,
        description: String(f.description || '').trim(),
        users: f.users,
        acl: f.acl.map((a) => ({
          agent_group_id: a.agent_group_id,
          tcp_ports: String(a.tcp_ports || '').trim(),
          tty: String(a.tty || '').trim(),
          file: a.file || 'none',
          shell: !!a.shell,
          sysinfo: !!a.sysinfo,
          metrics: !!a.metrics,
          config: !!a.config,
          web: !!a.web,
        })),
      }
    } else {
      base = 'agent-groups'
      body = {
        name,
        description: String(f.description || '').trim(),
        devices: f.devices,
        caps: {
          tcp_ports: String(f.caps.tcp_ports || '').trim(),
          tty: String(f.caps.tty || '').trim(),
          file: f.caps.file || 'none',
          shell: !!f.caps.shell,
          sysinfo: !!f.caps.sysinfo,
          metrics: !!f.caps.metrics,
          config: !!f.caps.config,
          web: !!f.caps.web,
        },
      }
    }
    if (f.id) await Api.put('/api/v1/permission/' + base + '/' + f.id, body)
    else await Api.post('/api/v1/permission/' + base, body)
    ElMessage.success('已保存')
    editing.value = null
    await load(true)
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function removeGroup(row) {
  try {
    await ElMessageBox.confirm('删除分组「' + row.name + '」？', '删除分组',
      { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    const base = tab.value === 'client' ? 'client-groups' : 'agent-groups'
    await Api.del('/api/v1/permission/' + base + '/' + row.id)
    if (isOpen(row)) editing.value = null
    ElMessage.success('已删除')
    await load(true)
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

// ---------------------------------------------------------------- 设备选择
const candidates = ref([])
const devDlg = ref(false)
const devQ = ref('')
const devPick = ref([])

const filteredCandidates = computed(() => {
  const k = devQ.value.trim().toLowerCase()
  return k ? candidates.value.filter((d) => d.devid.toLowerCase().includes(k)) : candidates.value
})

async function loadCandidates(id) {
  try {
    const r = await Api.get('/api/v1/permission/agent-groups/' + (id || 0) + '/candidates')
    candidates.value = r.devices || []
  } catch (e) {
    candidates.value = []
  }
}

function openDevicePicker() {
  devQ.value = ''
  devPick.value = []
  devDlg.value = true
}

function confirmDevices() {
  const have = new Set(editing.value.devices)
  devPick.value.forEach((d) => { if (!have.has(d)) editing.value.devices.push(d) })
  devDlg.value = false
}

function removeDevice(d) {
  editing.value.devices = editing.value.devices.filter((x) => x !== d)
}

// ---------------------------------------------------------------- 字典
const dictDlg = ref(false)
const dictTab = ref('ports')
const dictList = ref([])
const dictQ = ref('')
const dictLoading = ref(false)
const dictNew = reactive({ value: '', name: '' })
const dictEditDlg = ref(false)
const dictEdit = reactive({ id: 0, value: '', name: '', description: '', enabled: true })
const batchDlg = ref(false)
const batchText = ref('')
const batchPrefix = ref('')

const filteredDict = computed(() => {
  const k = dictQ.value.trim().toLowerCase()
  if (!k) return dictList.value
  return dictList.value.filter((d) =>
    String(d.value).toLowerCase().includes(k) || String(d.name || '').toLowerCase().includes(k))
})

function openDict() {
  dictQ.value = ''
  dictNew.value = ''
  dictNew.name = ''
  dictDlg.value = true
  loadDict()
}

async function loadDict() {
  dictLoading.value = true
  try {
    dictList.value = await Api.get('/api/v1/permission/dict/' + dictTab.value)
  } catch (e) {
    ElMessage.error('字典加载失败：' + e.message)
  } finally {
    dictLoading.value = false
  }
}

async function addDict() {
  const value = dictNew.value.trim()
  if (!value) {
    ElMessage.warning('请输入值')
    return
  }
  try {
    await Api.post('/api/v1/permission/dict/' + dictTab.value, { value, name: dictNew.name.trim() })
    dictNew.value = ''
    dictNew.name = ''
    await loadDict()
  } catch (e) {
    ElMessage.error('新增失败：' + e.message)
  }
}

function openDictEdit(row) {
  Object.assign(dictEdit, {
    id: row.id, value: row.value, name: row.name || '',
    description: row.description || '', enabled: !!row.enabled,
  })
  dictEditDlg.value = true
}

async function saveDictEdit() {
  try {
    await Api.put('/api/v1/permission/dict/' + dictTab.value + '/' + dictEdit.id, {
      name: dictEdit.name.trim(), description: dictEdit.description.trim(), enabled: !!dictEdit.enabled,
    })
    dictEditDlg.value = false
    ElMessage.success('已保存')
    await loadDict()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  }
}

async function removeDict(row) {
  try {
    if (row.refs > 0) {
      await ElMessageBox.confirm('该项被 ' + row.refs + ' 处 ACL 引用，不能删除。是否停用？', '无法删除',
        { type: 'warning', confirmButtonText: '停用', cancelButtonText: '取消' })
      await Api.put('/api/v1/permission/dict/' + dictTab.value + '/' + row.id, {
        name: row.name || '', description: row.description || '', enabled: false,
      })
    } else {
      await ElMessageBox.confirm('删除「' + row.value + '」？', '删除字典项',
        { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' })
      await Api.del('/api/v1/permission/dict/' + dictTab.value + '/' + row.id)
    }
    ElMessage.success('已处理')
    await loadDict()
  } catch (e) {
    if (e !== 'cancel' && e !== 'close') ElMessage.error('操作失败：' + e.message)
  }
}

async function batchAdd() {
  const values = batchText.value.split(/[,，\s\n]+/).map((x) => x.trim()).filter(Boolean)
  if (!values.length) {
    ElMessage.warning('请输入要添加的值')
    return
  }
  try {
    const r = await Api.post('/api/v1/permission/dict/' + dictTab.value + '/batch', {
      values, name_prefix: batchPrefix.value.trim(),
    })
    ElMessage.success('已添加 ' + (r.added || []).length + ' 项' +
      ((r.existing || []).length ? '，跳过 ' + r.existing.length + ' 项' : ''))
    batchDlg.value = false
    batchText.value = ''
    batchPrefix.value = ''
    await loadDict()
  } catch (e) {
    ElMessage.error('批量添加失败：' + e.message)
  }
}

// ---------------------------------------------------------------- 有效权限
const effDlg = ref(false)
const effClient = ref('')
const eff = ref(null)
const effLoading = ref(false)

function openEffective() {
  effDlg.value = true
  eff.value = null
  if (!effClient.value && accounts.value.length) effClient.value = accounts.value[0].username
}

async function loadEffective() {
  const user = effClient.value.trim()
  if (!user) {
    ElMessage.warning('请输入客户端用户名')
    return
  }
  effLoading.value = true
  try {
    eff.value = await Api.get('/api/v1/permission/effective?client=' + encodeURIComponent(user))
  } catch (e) {
    eff.value = null
    ElMessage.error('查询失败：' + e.message)
  } finally {
    effLoading.value = false
  }
}

// ---------------------------------------------------------------- 旧 ACL
const legacyDlg = ref(false)
const legacyText = ref('')

async function openLegacy() {
  legacyDlg.value = true
  await exportLegacy()
}

async function exportLegacy() {
  try {
    const r = await Api.get('/api/v1/permission/export-acl')
    legacyText.value = r.content || ''
  } catch (e) {
    ElMessage.error('导出失败：' + e.message)
  }
}

function downloadLegacy() {
  const blob = new Blob([legacyText.value], { type: 'text/plain' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = 'rtunnel-acl.txt'
  a.click()
  URL.revokeObjectURL(a.href)
}

async function importLegacy() {
  if (!legacyText.value.trim()) {
    ElMessage.warning('内容为空')
    return
  }
  try {
    await ElMessageBox.confirm('导入会创建/追加客户端分组，是否继续？', '导入旧 ACL',
      { type: 'warning', confirmButtonText: '导入', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    const r = await Api.post('/api/v1/permission/import-acl', { content: legacyText.value })
    ElMessage.success('已导入 ' + (r.imported || 0) + ' 条，设备 ' + (r.devices || 0) + ' 台')
    legacyDlg.value = false
    await load(true)
  } catch (e) {
    ElMessage.error('导入失败：' + e.message)
  }
}

onMounted(() => {
  load()
  loadAccounts()
})
</script>

<style scoped>
.cnt {
  display: inline-block; min-width: 18px; padding: 0 6px; margin-left: 4px;
  border-radius: 9px; background: var(--el-fill-color);
  color: var(--el-text-color-secondary); font-size: 11px; line-height: 18px; text-align: center;
}
.chev { display: inline-block; width: 14px; color: var(--el-text-color-secondary); }
.caps { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.editor { border-color: var(--el-color-primary-light-5); }
.form-grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 12px 16px; }
.form-grid > label { display: flex; flex-direction: column; gap: 6px; font-size: 12px; color: var(--el-text-color-secondary); }
h3 { font-size: 13px; margin: 18px 0 8px; }
.w100 { width: 100%; }
.grow { flex: 1; min-width: 0; }
.w160 { width: 160px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.empty-acl { margin-bottom: 8px; }
.mt6 { margin-top: 6px; }
.mt10 { margin-top: 10px; }
.mb6 { margin-bottom: 6px; }
.mb10 { margin-bottom: 10px; }
.acl-table :deep(.el-table__cell) { padding: 4px 0; }
.acl-cards { display: flex; flex-direction: column; gap: 10px; }
.acl-card { border: 1px solid var(--el-border-color-light); border-radius: 6px; padding: 10px; }
.acl-card-head { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
.acl-card-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px 10px; }
.acl-card-grid > label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--el-text-color-secondary); }
.checks { display: flex; flex-wrap: wrap; gap: 4px 14px; margin-top: 10px; }
.editor-foot { display: flex; align-items: center; justify-content: flex-end; gap: 10px; margin-top: 18px; }
.ro-hint { margin-right: auto; }
.cand-list { max-height: 380px; overflow: auto; }
.cand-item { display: flex; align-items: center; gap: 8px; width: 100%; margin-right: 0; padding: 4px 0; }
.dict-add { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; }
.eff-row { display: flex; gap: 8px; }
:deep(.perm-open) { background: var(--el-color-primary-light-9); cursor: pointer; }
:deep(.el-table__row) { cursor: pointer; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
  .form-grid { grid-template-columns: 1fr; }
  .acl-card-grid { grid-template-columns: 1fr 1fr; }
  .dict-add .w160 { width: 100%; }
}
</style>
