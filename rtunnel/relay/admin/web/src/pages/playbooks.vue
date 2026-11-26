<template>
  <div class="panel">
    <div class="panel-head">
      <h2>自愈规则</h2>
      <span class="muted">共 {{ rules.length }} 条</span>
      <span class="spacer" />
      <el-button :loading="loading" @click="loadAll">刷新</el-button>
      <el-button v-if="canWrite" type="primary" @click="openCreate">新建规则</el-button>
    </div>

    <el-table :data="rules" v-loading="loading" row-key="id">
      <el-table-column prop="id" label="ID" width="64" />
      <el-table-column label="名称" min-width="130" prop="name" />
      <el-table-column label="触发" width="120">
        <template #default="{ row }">
          <el-tag size="small" effect="plain">{{ eventText(row.event) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="设备" width="110" prop="devid_glob" />
      <el-table-column label="命令" min-width="200">
        <template #default="{ row }">
          <span class="cmd">{{ cmdline(row.argv) }}</span>
          <div v-if="row.verify_argv && row.verify_argv.length" class="muted">
            复核：{{ cmdline(row.verify_argv) }}
          </div>
        </template>
      </el-table-column>
      <el-table-column label="风险" width="90">
        <template #default="{ row }">
          <el-tag :type="riskType(row.risk)" size="small">{{ riskText(row.risk) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="自动执行" width="96">
        <template #default="{ row }">
          <el-tag v-if="row.auto_apply" :type="row.risk <= autoMaxRisk ? 'success' : 'warning'" size="small">
            {{ row.risk <= autoMaxRisk ? '自动' : '需审批' }}
          </el-tag>
          <span v-else class="muted">关</span>
        </template>
      </el-table-column>
      <el-table-column label="限流" width="130">
        <template #default="{ row }">
          <span class="muted">{{ row.cooldown_s }}s · {{ row.max_per_hour }}/时</span>
        </template>
      </el-table-column>
      <el-table-column label="启用" width="76">
        <template #default="{ row }">
          <el-switch :model-value="row.enabled" :disabled="!canWrite" @change="toggle(row, $event)" />
        </template>
      </el-table-column>
      <el-table-column label="操作" width="170">
        <template #default="{ row }">
          <el-button v-if="canWrite" link type="primary" @click="runNow(row)">立即执行</el-button>
          <el-button v-if="canWrite" link @click="openEdit(row)">编辑</el-button>
          <el-button v-if="canWrite" link type="danger" @click="remove(row)">删除</el-button>
        </template>
      </el-table-column>
      <template #empty><span class="muted">还没有规则。规则只做一件事：告警匹配后生成一个操作作业。</span></template>
    </el-table>

    <div class="hint muted">
      自动执行有三把锁：规则自身开启「自动执行」、命令风险 ≤ 系统设置
      <code>playbook.auto_apply_max_risk</code>（当前 {{ autoMaxRisk }}）、全局
      <code>playbook.enabled</code> 打开。任何一条不满足都只生成待批作业，不会自己动手。
      命令风险由服务端按 argv 计算，保存时传 risk 无效。
    </div>
  </div>

  <div class="panel">
    <div class="panel-head">
      <h2>触发记录</h2>
      <span class="muted">{{ fires.length }} 条</span>
      <span class="spacer" />
      <el-select v-model="fireStatus" clearable placeholder="全部状态" class="sel" @change="loadFires">
        <el-option label="已复核" value="verified" />
        <el-option label="已执行" value="applied" />
        <el-option label="待审批" value="pending_approval" />
        <el-option label="待执行" value="planned" />
        <el-option label="失败" value="failed" />
      </el-select>
    </div>
    <el-table :data="shownFires" v-loading="loading" row-key="op_id">
      <el-table-column label="时间" width="150">
        <template #default="{ row }">{{ time(row.created_ms) }}</template>
      </el-table-column>
      <el-table-column label="规则" min-width="120" prop="playbook" />
      <el-table-column label="设备" width="110" prop="devid" />
      <el-table-column label="命令" min-width="180">
        <template #default="{ row }"><span class="cmd">{{ cmdline(row.argv) }}</span></template>
      </el-table-column>
      <el-table-column label="状态" width="110">
        <template #default="{ row }">
          <el-tag :type="statusType(row.status)" size="small" :title="row.error || ''">
            {{ statusText(row.status) }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="作业" width="86">
        <template #default="{ row }">#{{ row.op_id }}</template>
      </el-table-column>
      <el-table-column label="错误" min-width="150" show-overflow-tooltip>
        <template #default="{ row }"><span class="err">{{ row.error || '' }}</span></template>
      </el-table-column>
      <template #empty><span class="muted">还没有触发记录</span></template>
    </el-table>
  </div>

  <el-dialog v-model="dlg" :title="form.id ? '编辑规则 #' + form.id : '新建规则'" width="560px">
    <el-form :model="form" label-width="96px">
      <el-form-item label="名称" required>
        <el-input v-model="form.name" maxlength="64" placeholder="例如：上线自检" />
      </el-form-item>
      <el-form-item label="触发事件">
        <el-select v-model="form.event" style="width: 100%">
          <el-option v-for="e in EVENTS" :key="e.value" :label="e.label" :value="e.value" />
        </el-select>
      </el-form-item>
      <el-form-item label="设备匹配">
        <el-input v-model="form.devid_glob" placeholder="* 或 dev*、mp135" />
      </el-form-item>
      <el-form-item label="命令" required>
        <el-input v-model="form.argvText" type="textarea" :rows="3"
                  placeholder="每行一段，例如：&#10;systemctl&#10;restart&#10;nginx" />
        <div class="muted">按 argv 逐段执行，不经过 shell；需在 agent 的 --allow-exec 白名单内。</div>
      </el-form-item>
      <el-form-item label="复核命令">
        <el-input v-model="form.verifyText" type="textarea" :rows="2"
                  placeholder="可选，每行一段；exit_code=0 才算自愈成功" />
      </el-form-item>
      <el-form-item label="自动执行">
        <el-switch v-model="form.auto_apply" />
        <span class="muted ml8">超过全局风险上限时仍会转为待审批</span>
      </el-form-item>
      <el-form-item label="冷却">
        <el-input-number v-model="form.cooldown_s" :min="0" :max="86400" :step="60" />
        <span class="unit">秒（0 = 不冷却）</span>
      </el-form-item>
      <el-form-item label="每小时上限">
        <el-input-number v-model="form.max_per_hour" :min="0" :max="1000" />
        <span class="unit">次（0 = 不限）</span>
      </el-form-item>
      <el-form-item label="备注">
        <el-input v-model="form.note" maxlength="256" placeholder="可选" />
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="dlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="save">保存</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { time } from '@/shared/utils.js'

const rules = ref([])
const fires = ref([])
const fireStatus = ref('')
const loading = ref(false)
const saving = ref(false)
const dlg = ref(false)
const autoMaxRisk = ref(1)

const canWrite = computed(() => can('playbook.write'))

const EVENTS = [
  { value: 'device.online', label: '设备上线' },
  { value: 'device.offline', label: '设备离线' },
  { value: 'task.failed', label: '任务失败' },
  { value: 'cert.expiring', label: '证书将过期' },
  { value: '*', label: '任意告警' },
]

const form = reactive({
  id: 0, name: '', event: 'device.online', devid_glob: '*',
  argvText: '', verifyText: '', auto_apply: false,
  cooldown_s: 300, max_per_hour: 6, note: '',
})

function eventText(e) {
  const f = EVENTS.find((x) => x.value === e)
  return f ? f.label : e
}

function cmdline(argv) {
  const s = (argv || []).join(' ')
  return s.length > 70 ? s.slice(0, 70) + '…' : (s || '—')
}

function riskText(r) { return r >= 3 ? '破坏性' : (r === 2 ? '改状态' : '只读') }
function riskType(r) { return r >= 3 ? 'danger' : (r === 2 ? 'warning' : 'info') }

const STATUS_TEXT = {
  planned: '待执行', pending_approval: '待审批', approved: '已批准',
  running: '执行中', applied: '已执行', verified: '已复核', failed: '失败', rejected: '已拒绝',
}
const STATUS_TYPE = {
  planned: 'info', pending_approval: 'warning', approved: 'primary',
  running: 'warning', applied: 'success', verified: 'success', failed: 'danger', rejected: 'danger',
}
function statusText(s) { return STATUS_TEXT[s] || s }
function statusType(s) { return STATUS_TYPE[s] || 'info' }

const shownFires = computed(() =>
  fireStatus.value ? fires.value.filter((x) => x.status === fireStatus.value) : fires.value)

async function loadRules() {
  const r = await Api.get('/api/v1/playbooks')
  rules.value = r.items || []
}

async function loadFires() {
  const r = await Api.get('/api/v1/playbooks/fires?limit=100')
  fires.value = r.items || []
}

async function loadAutoMax() {
  if (!can('settings.read')) return
  const s = await Api.get('/api/v1/settings').catch(() => null)
  const v = s && s['playbook.auto_apply_max_risk']
  if (v !== undefined && v !== null) autoMaxRisk.value = Number(v) || 1
}

async function loadAll() {
  loading.value = true
  try {
    await Promise.all([loadRules(), loadFires(), loadAutoMax()])
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function openCreate() {
  Object.assign(form, {
    id: 0, name: '', event: 'device.online', devid_glob: '*',
    argvText: '', verifyText: '', auto_apply: false,
    cooldown_s: 300, max_per_hour: 6, note: '',
  })
  dlg.value = true
}

function openEdit(row) {
  Object.assign(form, {
    id: row.id, name: row.name, event: row.event, devid_glob: row.devid_glob,
    argvText: (row.argv || []).join('\n'),
    verifyText: (row.verify_argv || []).join('\n'),
    auto_apply: !!row.auto_apply,
    cooldown_s: row.cooldown_s, max_per_hour: row.max_per_hour, note: row.note || '',
  })
  dlg.value = true
}

function lines(text) {
  return String(text || '').split('\n').map((s) => s.trim()).filter(Boolean)
}

async function save() {
  const argv = lines(form.argvText)
  if (!form.name.trim()) { ElMessage.warning('请填写名称'); return }
  if (!argv.length) { ElMessage.warning('命令不能为空'); return }
  saving.value = true
  try {
    const body = {
      name: form.name.trim(), event: form.event, devid_glob: form.devid_glob.trim() || '*',
      argv, verify_argv: lines(form.verifyText),
      auto_apply: form.auto_apply, cooldown_s: form.cooldown_s,
      max_per_hour: form.max_per_hour, note: form.note.trim(), enabled: true,
    }
    if (form.id) await Api.put('/api/v1/playbooks/' + form.id, body)
    else await Api.post('/api/v1/playbooks', body)
    ElMessage.success('已保存')
    dlg.value = false
    await loadAll()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function toggle(row, val) {
  try {
    await Api.post('/api/v1/playbooks/' + row.id + '/enable', { enabled: !!val })
    row.enabled = !!val
  } catch (e) {
    ElMessage.error('切换失败：' + e.message)
    await loadRules()
  }
}

async function remove(row) {
  try {
    await ElMessageBox.confirm('删除后历史触发记录仍保留，确认删除「' + row.name + '」？', '删除规则', {
      confirmButtonText: '删除', cancelButtonText: '取消', type: 'warning',
    })
  } catch (e) {
    return
  }
  try {
    await Api.del('/api/v1/playbooks/' + row.id)
    ElMessage.success('已删除')
    await loadAll()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

async function runNow(row) {
  try {
    const r = await ElMessageBox.prompt(
      '按规则命令在设备上生成一个作业（仍走审批与自动执行开关）', '立即执行「' + row.name + '」', {
        confirmButtonText: '生成作业', cancelButtonText: '取消',
        inputPlaceholder: '设备 ID', inputValidator: (v) => (v && v.trim() ? true : '必须填写设备 ID'),
      })
    const op = await Api.post('/api/v1/playbooks/' + row.id + '/run', { devid: r.value.trim() })
    const st = statusText(op.status)
    ElMessage.success('已生成作业 #' + op.id + '（' + st + '）')
    await loadAll()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('执行失败：' + (e.message || e))
  }
}

let timer = null
onMounted(() => {
  loadAll()
  timer = setInterval(() => { if (!document.hidden) loadFires() }, 5000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.sel { width: 150px; }
.cmd { font-family: var(--el-font-family-mono, monospace); font-size: 12.5px; }
.unit { margin-left: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.ml8 { margin-left: 8px; }
.hint { margin-top: 10px; font-size: 12.5px; line-height: 1.7; }
.err { color: var(--el-color-danger); }
@media (max-width: 599px) {
  .sel { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
