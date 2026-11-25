<template>
  <div class="panel">
    <div class="panel-head">
      <h2>费用</h2>
      <span class="muted">用量 × 单价试算 · 合计 {{ money(totalMicro) }}</span>
      <span class="spacer" />
      <el-select v-if="can('tenant.read')" v-model="tenantId" style="width: 150px" @change="reloadAll">
        <el-option label="全部租户" :value="0" />
        <el-option v-for="t in tenants" :key="t.id" :label="t.name" :value="t.id" />
      </el-select>
      <el-date-picker v-model="range" type="daterange" value-format="YYYY-MM-DD"
                      start-placeholder="开始日" end-placeholder="结束日"
                      style="width: 240px" @change="load" />
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>

    <el-table :data="rows" v-loading="loading" size="small">
      <el-table-column prop="tenant_name" label="租户" min-width="130" />
      <el-table-column label="指标" width="140">
        <template #default="{ row }">{{ metricLabel(row.metric) }}</template>
      </el-table-column>
      <el-table-column label="用量" width="150" align="right">
        <template #default="{ row }">{{ fmtQty(row.qty, row.metric) }}</template>
      </el-table-column>
      <el-table-column label="计价单位" width="150">
        <template #default="{ row }">{{ unitText(row) }}</template>
      </el-table-column>
      <el-table-column label="单价" width="130" align="right">
        <template #default="{ row }">{{ priceText(row) }}</template>
      </el-table-column>
      <el-table-column label="金额" width="130" align="right">
        <template #default="{ row }">{{ money(row.amount_micro) }}</template>
      </el-table-column>
      <template #empty><el-empty description="所选范围内没有用量" :image-size="70" /></template>
    </el-table>

    <div class="hint muted">
      金额按「用量 ÷ 计价单位 × 单价」向下取整到 0.000001，仅用于核算，不生成账单/发票。
      日按 relay 本地时区，用量来自「监控 → 用量」的日聚合。
    </div>
  </div>

  <div class="panel">
    <div class="panel-head">
      <h2>价目表</h2>
      <span class="muted">未设置单价的指标按 0 计算</span>
    </div>
    <el-table :data="prices" size="small">
      <el-table-column label="指标" width="160">
        <template #default="{ row }">{{ metricLabel(row.metric) }}</template>
      </el-table-column>
      <el-table-column label="计价单位" width="160">
        <template #default="{ row }">{{ unitText({ metric: row.metric, unit_size: row.unit_size }) }}</template>
      </el-table-column>
      <el-table-column label="单价" width="160">
        <template #default="{ row }">{{ priceText({ metric: row.metric, price_micro: row.price_micro }) }}</template>
      </el-table-column>
      <el-table-column prop="currency" label="币种" width="80" />
      <el-table-column label="操作" width="90">
        <template #default="{ row }">
          <el-button v-if="can('billing.write')" link type="primary" @click="openPrice(row)">调整</el-button>
          <span v-else class="muted">—</span>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无价目" :image-size="60" /></template>
    </el-table>
  </div>

  <div class="panel">
    <div class="panel-head">
      <h2>结算单</h2>
      <span class="muted">按账期冻结费用快照，出账后不再随用量变化</span>
      <span class="spacer" />
      <el-date-picker v-model="stmtPeriod" type="month" value-format="YYYY-MM" placeholder="账期"
                      style="width: 130px" @change="loadStatements" />
      <el-select v-model="stmtStatus" style="width: 120px" @change="loadStatements">
        <el-option label="全部状态" value="" />
        <el-option label="草稿" value="draft" />
        <el-option label="已出账" value="issued" />
        <el-option label="已收款" value="paid" />
        <el-option label="已作废" value="void" />
      </el-select>
      <el-button v-if="can('billing.write')" type="primary" :loading="genLoading"
                 @click="generateStatements">生成 / 重算</el-button>
      <el-button :loading="stmtLoading" @click="loadStatements">刷新</el-button>
    </div>

    <el-table :data="statements" v-loading="stmtLoading" size="small">
      <el-table-column type="expand">
        <template #default="{ row }">
          <div class="lines">
            <el-table :data="row.lines" size="small" border>
              <el-table-column label="指标" width="140">
                <template #default="s">{{ metricLabel(s.row.metric) }}</template>
              </el-table-column>
              <el-table-column label="用量" width="140" align="right">
                <template #default="s">{{ fmtQty(s.row.qty, s.row.metric) }}</template>
              </el-table-column>
              <el-table-column label="计价单位" width="140">
                <template #default="s">{{ unitText(s.row) }}</template>
              </el-table-column>
              <el-table-column label="单价" width="120" align="right">
                <template #default="s">{{ priceText(s.row) }}</template>
              </el-table-column>
              <el-table-column label="金额" width="120" align="right">
                <template #default="s">{{ money(s.row.amount_micro) }}</template>
              </el-table-column>
            </el-table>
          </div>
        </template>
      </el-table-column>
      <el-table-column prop="tenant_name" label="租户" min-width="120" />
      <el-table-column prop="period" label="账期" width="90" />
      <el-table-column label="金额" width="120" align="right">
        <template #default="{ row }">{{ money(row.total_micro) }}</template>
      </el-table-column>
      <el-table-column label="状态" width="90">
        <template #default="{ row }">
          <el-tag :type="statusTag(row.status)" size="small">{{ statusText(row.status) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="note" label="备注" min-width="120" show-overflow-tooltip />
      <el-table-column label="操作" width="210">
        <template #default="{ row }">
          <el-button v-if="can('billing.write') && row.status === 'draft'" link type="primary"
                     @click="setStatus(row, 'issued')">出账</el-button>
          <el-button v-if="can('billing.write') && row.status === 'issued'" link type="success"
                     @click="setStatus(row, 'paid')">已收款</el-button>
          <el-button v-if="can('billing.write') && (row.status === 'draft' || row.status === 'issued')"
                     link type="danger" @click="setStatus(row, 'void')">作废</el-button>
          <el-button link @click="exportCsv(row)">导出</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="该账期还没有结算单" :image-size="60" /></template>
    </el-table>

    <div class="hint muted">
      生成只处理草稿：已有草稿按最新用量重算，已出账的记录跳过（需要调整先作废）。
      出账即冻结明细，之后价目或用量变化不影响这张单。
    </div>
  </div>

  <el-dialog v-model="dlg" :title="'调整单价 · ' + metricLabel(form.metric)" width="440px">
    <el-form :model="form" label-width="96px">
      <el-form-item label="计价单位">
        <el-input-number v-model="form.unit_size" :min="1" :step="1" style="width: 160px" />
        <span class="unit">{{ unitName(form.metric) }}</span>
      </el-form-item>
      <el-form-item label="单价（元）">
        <el-input-number v-model="form.price_yuan" :min="0" :precision="6" :step="0.1" style="width: 160px" />
        <span class="unit">/ {{ unitText(form) }}</span>
      </el-form-item>
      <el-form-item label="币种">
        <el-input v-model="form.currency" maxlength="8" style="width: 120px" />
      </el-form-item>
    </el-form>
    <template #footer>
      <el-button @click="dlg = false">取消</el-button>
      <el-button type="primary" :loading="saving" @click="savePrice">保存</el-button>
    </template>
  </el-dialog>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'

const METRICS = [
  { value: 'bytes', label: '隧道流量' },
  { value: 'sessions', label: '会话次数' },
  { value: 'session_seconds', label: '会话时长' },
  { value: 'online_seconds', label: '设备在线时长' },
]
// 计价单位的自然名称：流量按 GB，时长按小时/天，次数按千次
const UNIT_NAME = {
  bytes: 'GiB',
  sessions: '次',
  session_seconds: '小时',
  online_seconds: '天',
}

const rows = ref([])
const prices = ref([])
const tenants = ref([])
const tenantId = ref(0)
const loading = ref(false)
const totalMicro = ref(0)
const range = ref(defaultRange())

const dlg = ref(false)
const saving = ref(false)
const form = reactive({ metric: '', unit_size: 1, price_yuan: 0, currency: 'CNY' })

const statements = ref([])
const stmtLoading = ref(false)
const genLoading = ref(false)
const stmtStatus = ref('')
const stmtPeriod = ref(defaultPeriod())

function defaultPeriod() {
  const d = new Date()
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}`
}

function statusText(s) {
  return { draft: '草稿', issued: '已出账', paid: '已收款', void: '已作废' }[s] || s
}

function statusTag(s) {
  return { draft: 'info', issued: 'warning', paid: 'success', void: 'danger' }[s] || 'info'
}

function defaultRange() {
  const end = new Date()
  const start = new Date(end.getFullYear(), end.getMonth(), 1)
  const fmt = (d) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
  return [fmt(start), fmt(end)]
}

function metricLabel(key) {
  const m = METRICS.find((x) => x.value === key)
  return m ? m.label : key
}
function unitName(metric) {
  return UNIT_NAME[metric] || ''
}

function fmtBytes(n) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let v = Number(n) || 0
  let i = 0
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return (i === 0 ? String(v) : v.toFixed(v >= 100 ? 0 : 1)) + ' ' + units[i]
}

function fmtDuration(n) {
  let s = Math.round(Number(n) || 0)
  const d = Math.floor(s / 86400); s -= d * 86400
  const h = Math.floor(s / 3600); s -= h * 3600
  const m = Math.floor(s / 60); s -= m * 60
  if (d) return `${d}天${h}小时`
  if (h) return `${h}小时${m}分`
  if (m) return `${m}分${s}秒`
  return `${s}秒`
}

function fmtQty(qty, key) {
  if (key === 'bytes') return fmtBytes(qty)
  if (key === 'session_seconds' || key === 'online_seconds') return fmtDuration(qty)
  return String(qty)
}

function unitText(row) {
  const u = Number(row.unit_size) || 0
  const name = unitName(row.metric)
  if (row.metric === 'bytes') return fmtBytes(u)
  if (row.metric === 'session_seconds') return u === 3600 ? '1 小时' : u + ' 秒'
  if (row.metric === 'online_seconds') return u === 86400 ? '1 天' : u + ' 秒'
  return u + ' ' + name
}

function priceText(row) {
  if (!row.price_micro) return '未定价'
  return '¥' + (row.price_micro / 1000000).toFixed(6).replace(/0+$/, '').replace(/\.$/, '')
}

function money(micro) {
  return '¥' + ((Number(micro) || 0) / 1000000).toFixed(6).replace(/0+$/, '').replace(/\.$/, '')
}

async function load() {
  loading.value = true
  try {
    const q = new URLSearchParams()
    if (range.value && range.value.length === 2) {
      q.set('from', range.value[0])
      q.set('to', range.value[1])
    }
    if (tenantId.value > 0) q.set('tenant_id', String(tenantId.value))
    const res = await Api.get('/api/v1/billing/summary?' + q.toString())
    const flat = []
    for (const t of res.tenants || []) {
      for (const it of t.items || []) {
        flat.push({ ...it, tenant_id: t.tenant_id, tenant_name: t.tenant_name })
      }
    }
    rows.value = flat
    totalMicro.value = res.total_micro || 0
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function loadPrices() {
  try {
    const res = await Api.get('/api/v1/billing/prices')
    prices.value = (res && res.items) || []
  } catch (e) {
    ElMessage.error('价目加载失败：' + e.message)
  }
}

async function loadTenants() {
  if (!can('tenant.read')) return
  const res = await Api.get('/api/v1/tenants').catch(() => null)
  tenants.value = (res && res.items) || []
}

async function loadStatements() {
  stmtLoading.value = true
  try {
    const q = new URLSearchParams()
    if (stmtPeriod.value) q.set('period', stmtPeriod.value)
    if (stmtStatus.value) q.set('status', stmtStatus.value)
    if (tenantId.value > 0) q.set('tenant_id', String(tenantId.value))
    const res = await Api.get('/api/v1/billing/statements?' + q.toString())
    statements.value = (res && res.items) || []
  } catch (e) {
    ElMessage.error('结算单加载失败：' + e.message)
  } finally {
    stmtLoading.value = false
  }
}

async function generateStatements() {
  genLoading.value = true
  try {
    const res = await Api.post('/api/v1/billing/statements', {
      period: stmtPeriod.value || defaultPeriod(),
      tenant_id: tenantId.value || 0,
    })
    const n = (a) => (a || []).length
    ElMessage.success(`新建 ${n(res.created)}、重算 ${n(res.updated)}、跳过 ${n(res.skipped)}`)
    await loadStatements()
  } catch (e) {
    ElMessage.error('生成失败：' + e.message)
  } finally {
    genLoading.value = false
  }
}

async function setStatus(row, status) {
  if (status === 'void') {
    try {
      await ElMessageBox.confirm('作废后不能恢复，已作废的账期可以重新生成草稿。', '作废结算单',
                                 { type: 'warning' })
    } catch {
      return
    }
  }
  try {
    await Api.put('/api/v1/billing/statements/' + row.id, { status })
    ElMessage.success('已更新')
    await loadStatements()
  } catch (e) {
    ElMessage.error('更新失败：' + e.message)
  }
}

function reloadAll() {
  load()
  loadStatements()
}

function exportCsv(row) {
  window.open('/api/v1/billing/statements/' + row.id + '/csv', '_blank')
}

function openPrice(row) {
  form.metric = row.metric
  form.unit_size = Number(row.unit_size) || 1
  form.price_yuan = Number(row.price_micro || 0) / 1000000
  form.currency = row.currency || 'CNY'
  dlg.value = true
}

async function savePrice() {
  saving.value = true
  try {
    await Api.put('/api/v1/billing/prices/' + encodeURIComponent(form.metric), {
      unit_size: Number(form.unit_size) || 1,
      price_yuan: Number(form.price_yuan) || 0,
      currency: form.currency.trim() || 'CNY',
    })
    ElMessage.success('价目已保存')
    dlg.value = false
    await Promise.all([loadPrices(), load()])
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  loadTenants()
  loadPrices()
  load()
  loadStatements()
})
</script>

<style scoped>
.unit { margin-left: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.hint { margin-top: 10px; font-size: 12.5px; line-height: 1.7; }
.lines { padding: 4px 16px 8px; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; row-gap: 6px; }
}
</style>
