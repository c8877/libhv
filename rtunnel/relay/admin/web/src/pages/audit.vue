<template>
  <div class="panel">
    <div class="panel-head">
      <h2>审计日志</h2>
      <span class="muted">最近 {{ rows.length }} 条（新到旧，relay 本地时间）</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索操作 / 对象 / 详情" clearable class="w220">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button :loading="loading" @click="load">刷新</el-button>
      <el-button :disabled="!filtered.length" @click="exportCsv">导出 CSV</el-button>
      <a ref="dlEl" class="dl" :href="dlHref" :download="dlName"></a>
    </div>

    <div class="bar">
      <el-radio-group v-model="resultFilter" size="small">
        <el-radio-button value="">全部 {{ rows.length }}</el-radio-button>
        <el-radio-button value="ok">成功 {{ countOf('ok') }}</el-radio-button>
        <el-radio-button value="denied">拒绝 {{ countOf('denied') }}</el-radio-button>
        <el-radio-button value="error">错误 {{ countOf('error') }}</el-radio-button>
      </el-radio-group>
      <span class="spacer" />
      <span class="muted">{{ filtered.length }} 条匹配</span>
    </div>

    <el-table :data="pageRows" v-loading="loading" size="small" row-key="key"
              @sort-change="onSortChange">
      <template v-if="isPhone">
        <el-table-column label="事件">
          <template #default="{ row }">
            <div class="ev">
              <b>{{ actionLabel(row.action) }}</b>
              <el-tag :type="resultLabel(row.result).type" size="small" effect="plain">
                {{ resultLabel(row.result).text }}
              </el-tag>
            </div>
            <div class="ev-sub mono">{{ row.action }}{{ row.target ? ' · ' + row.target : '' }}</div>
            <div class="ev-sub">{{ row.detail }}</div>
            <div class="ev-sub muted">{{ row.local_at }} · {{ row.actor }}</div>
          </template>
        </el-table-column>
      </template>
      <template v-else>
        <el-table-column label="时间" width="170" prop="at" sortable="custom"
                         :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <span class="muted" :title="row.at + '（UTC）'">{{ row.local_at }}</span>
          </template>
        </el-table-column>
        <el-table-column label="操作者" min-width="190" prop="actor" sortable="custom"
                         :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">
            <div class="who">{{ row.actor }}</div>
            <el-tag v-if="row.actor_type" size="small" type="info" effect="plain">
              {{ actorLabel(row.actor_type) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" min-width="180" prop="action" sortable="custom"
                         :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">
            <div>{{ actionLabel(row.action) }}</div>
            <div class="muted mono">{{ row.action }}</div>
          </template>
        </el-table-column>
        <el-table-column label="对象" min-width="150" prop="target" sortable="custom"
                         :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">
            <span v-if="row.target" class="mono">{{ row.target }}</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <el-table-column label="结果" width="90" prop="result" sortable="custom"
                         :sort-orders="TEXT_ORDERS">
          <template #default="{ row }">
            <el-tag :type="resultLabel(row.result).type" size="small" effect="plain">
              {{ resultLabel(row.result).text }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="详情" min-width="240">
          <template #default="{ row }">
            <span :title="row.detail">{{ row.detail.length > 90 ? row.detail.slice(0, 89) + '…' : row.detail }}</span>
          </template>
        </el-table-column>
      </template>
      <template #empty>
        <el-empty :description="rows.length ? '没有符合条件的记录' : '暂无审计记录'" :image-size="70" />
      </template>
    </el-table>

    <div class="foot">
      <el-pagination v-model:current-page="page" :page-size="per" :total="filtered.length"
                     layout="prev, pager, next, total" background />
      <span class="spacer" />
      <span class="muted">内存保留最近 1000 条；历史行同时写在 relay 的审计文件里</span>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, reactive, ref, watch } from 'vue'
import { Api } from '@/shared/api.js'
import { actionLabel, actorLabel, resultLabel } from '@/shared/auditLabels.js'
import { NUM_ORDERS, TEXT_ORDERS, sortByOrder } from '@/shared/tableSort.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()

const rows = ref([])
const q = ref('')
const loading = ref(false)
const resultFilter = ref('')
const page = ref(1)
const per = 50
const dlEl = ref(null)
const dlHref = ref('')
const dlName = ref('')

function pad(n) {
  return String(n).padStart(2, '0')
}

// 接口给的是 UTC（带 Z）。界面按 relay 所在时区显示，原始值留在 title 里。
function localTime(at) {
  if (!at) return '—'
  const d = new Date(at)
  if (isNaN(d.getTime())) return at
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
    `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

function parse(row, i) {
  const target = row.target_id ? (row.target_type ? row.target_type + ':' : '') + row.target_id : ''
  return {
    key: (row.at || '') + '#' + i,
    at: row.at || '',
    local_at: localTime(row.at),
    actor: row.actor || '',
    actor_type: row.actor_type || '',
    action: row.action || '',
    result: row.result || 'ok',
    target,
    detail: row.detail || '',
  }
}

const countOf = (v) => rows.value.filter((r) => r.result === v).length

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  return rows.value.filter((r) => {
    if (resultFilter.value && r.result !== resultFilter.value) return false
    if (!k) return true
    return [r.action, r.actor, r.detail, r.target].some((x) => String(x).toLowerCase().includes(k))
  })
})

// 排序走 el-table 的 custom：先排全量再分页，不然只排当前页等于没排
const sort = reactive({ prop: '', order: '' })
const sorted = computed(() => sortByOrder(filtered.value, sort.prop, sort.order, {
  result: (r) => r.result,
}))
const pageRows = computed(() => sorted.value.slice((page.value - 1) * per, page.value * per))

function onSortChange({ prop, order }) {
  sort.prop = prop || ''
  sort.order = order || ''
  page.value = 1
}

watch([q, resultFilter], () => { page.value = 1 })

async function load() {
  loading.value = true
  try {
    // 一次取满 ring 上限（1000），筛选与分页在本地做；接口默认仍然只回 200
    const arr = await Api.get('/api/v1/audit?limit=1000')
    rows.value = (arr || []).map(parse)
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function csvCell(v) {
  const s = String(v == null ? '' : v)
  return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s
}

async function exportCsv() {
  const head = ['时间(本地)', '时间(UTC)', '操作者', '类型', '操作', '操作键', '对象', '结果', '详情']
  const lines = [head.join(',')]
  for (const r of filtered.value) {
    lines.push([
      r.local_at, r.at, r.actor, actorLabel(r.actor_type), actionLabel(r.action),
      r.action, r.target, resultLabel(r.result).text, r.detail,
    ].map(csvCell).join(','))
  }
  const blob = new Blob(['\ufeff' + lines.join('\r\n')], { type: 'text/csv;charset=utf-8' })
  if (dlHref.value) URL.revokeObjectURL(dlHref.value)
  dlHref.value = URL.createObjectURL(blob)
  dlName.value = `audit-${new Date().toISOString().slice(0, 10)}.csv`
  // 下载走模板里的隐藏 <a>，不自己造节点
  await nextTick()
  dlEl.value && dlEl.value.click()
}

onMounted(load)
</script>

<style scoped>
.bar { display: flex; align-items: center; gap: 10px; margin: -4px 0 10px; flex-wrap: wrap; }
.foot { display: flex; align-items: center; gap: 10px; margin-top: 12px; flex-wrap: wrap; }
.foot .spacer { flex: 1; }
.who { line-height: 1.7; word-break: break-all; }
.dl { display: none; }
.ev { display: flex; align-items: center; gap: 8px; }
.ev-sub { margin-top: 2px; font-size: 12px; word-break: break-all; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
}
</style>
