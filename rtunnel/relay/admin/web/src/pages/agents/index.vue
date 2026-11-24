<template>
  <div class="panel">
    <div class="panel-head">
      <h2>设备（C 侧）</h2>
      <span class="muted">共 {{ list.length }} 台 · 5s 自动刷新</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索 devid / 地址" clearable class="search">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
    </div>

    <div ref="tableWrap">
      <el-table :data="filtered" v-loading="loading" row-class-name="row-click"
                @row-click="openRow" @row-contextmenu="openContextMenu">
        <el-table-column label="devid" min-width="140" prop="devid" sortable :sort-orders="TEXT_ORDERS">
          <template #default="{ row }"><b>{{ row.devid }}</b></template>
        </el-table-column>
        <el-table-column label="状态" width="90">
          <template #default><el-tag type="success" size="small">在线</el-tag></template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150"
                         sortable :sort-orders="TEXT_ORDERS" />
        <el-table-column v-if="!isPhone" label="最近活跃" min-width="120" prop="last_seen_ms"
                         sortable :sort-orders="NUM_ORDERS">
          <template #default="{ row }">{{ ago(row.last_seen_ms) }} 前活跃</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="系统运行" min-width="110" prop="uptime"
                         sortable :sort-by="(r) => (r.boot_ms ? nowMs - r.boot_ms : 0)" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <span v-if="row.boot_ms">{{ duration(nowMs - row.boot_ms) }}</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="在线时长" min-width="110" prop="online_ms"
                         sortable :sort-by="(r) => r.connected_at_ms || 0" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">
            <span v-if="row.connected_at_ms">{{ duration(nowMs - row.connected_at_ms) }}</span>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="streams" label="会话数" width="90" align="right"
                         sortable :sort-orders="NUM_ORDERS" />
        <el-table-column v-if="!isPhone" label="流量 ↓/↑" min-width="150" prop="traffic"
                         sortable :sort-by="(r) => (r.bytes_in || 0) + (r.bytes_out || 0)" :sort-orders="NUM_ORDERS">
          <template #default="{ row }">{{ bytes(row.bytes_in) }} / {{ bytes(row.bytes_out) }}</template>
        </el-table-column>
        <el-table-column :width="actionWidth" label="操作" align="center">
          <template #default="{ row }">
            <RowActions :items="actionsOf(row)" :dropdown-only="isPhone"
                        @action="(c) => onAction(c, row)" />
          </template>
        </el-table-column>
        <template #empty><el-empty description="暂无在线设备" :image-size="70" /></template>
      </el-table>
    </div>

    <el-dialog v-model="webDlg" :title="'打开 Web · ' + (webRow ? webRow.devid : '')"
               :width="isPhone ? '92%' : '520px'" append-to-body @closed="webRow = null">
      <div v-loading="webLoading" class="web-pick">
        <div class="web-tip muted">选择 agent 已开放的转发端口</div>
        <el-radio-group v-if="webChips.length" v-model="webPort" class="web-chips">
          <el-radio v-for="c in webChips" :key="c.port" :value="String(c.port)"
                    class="web-chip" :title="'打开 ' + c.port + ' 端口'">
            <span class="web-label">{{ c.label }}</span>
            <span v-if="c.sub" class="web-sub">{{ c.sub }}</span>
          </el-radio>
        </el-radio-group>
        <div v-else-if="!webLoading" class="web-empty muted">
          该设备尚未上报开放端口，可直接输入端口号
        </div>
        <el-input v-model="webPort" size="small" class="web-input" placeholder="例如 80">
          <template #prepend>端口</template>
        </el-input>
        <div v-if="validPort" class="web-note muted">
          将在新标签页打开 <code>/device/{{ webRow && webRow.devid }}/{{ webPort }}/</code>
        </div>
      </div>
      <template #footer>
        <el-button @click="webDlg = false">取消</el-button>
        <el-button type="primary" :disabled="!validPort" @click="confirmWeb">打开</el-button>
      </template>
    </el-dialog>

    <teleport to="body">
      <!-- ui-allow-begin: 右键菜单跟随光标坐标，el-dropdown 只按触发元素定位 -->
      <div v-if="ctx.open" class="ctx-menu" :style="{ left: ctx.x + 'px', top: ctx.y + 'px' }"
           @contextmenu.prevent @mousedown.stop>
        <div class="ctx-head">{{ ctx.row && ctx.row.devid }}</div>
        <button class="ctx-item" @click="ctxAction('detail')">详情</button>
        <button v-if="can('device.terminal') && ctx.row && ctx.row.caps_shell !== 0" class="ctx-item"
                @click="ctxAction('shell')">打开 Shell</button>
        <button v-if="can('device.port')" class="ctx-item" @click="ctxAction('web')">打开 Web</button>
        <div v-if="can('device.kick')" class="ctx-sep" />
        <button v-if="can('device.kick')" class="ctx-item danger" @click="ctxAction('kick')">强制下线</button>
      </div>
      <!-- ui-allow-end -->
    </teleport>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import RowActions from '@/components/RowActions.vue'
import { Api } from '@/shared/api.js'
import { can } from '@/shared/store.js'
import { ago, bytes, duration } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'
import { NUM_ORDERS, TEXT_ORDERS } from '@/shared/tableSort.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const list = ref([])
const q = ref('')
const loading = ref(false)
const nowMs = ref(Date.now())
const tableWrap = ref(null)
const tableW = ref(0)
// 其余列的最小宽度合计；剩余空间给操作列，放不下时由 RowActions 收进 ...
const OTHER_COL_MIN = 960
const actionWidth = computed(() => {
  if (isPhone.value) return 56
  return Math.max(56, Math.min(210, tableW.value - OTHER_COL_MIN))
})

function actionsOf(row) {
  const out = []
  if (can('device.terminal') && row.caps_shell !== 0) out.push({ key: 'shell', label: 'Shell' })
  if (can('device.port')) out.push({ key: 'web', label: 'Web' })
  if (can('device.kick')) out.push({ key: 'kick', label: '下线', danger: true })
  return out
}

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return list.value
  return list.value.filter((a) =>
    String(a.devid).toLowerCase().includes(k) ||
    String(a.remote_addr || '').toLowerCase().includes(k))
})

async function refresh(silent) {
  if (!silent) loading.value = true
  nowMs.value = Date.now()
  try {
    list.value = await Api.get('/api/v1/agents')
    nowMs.value = Date.now()
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onAction(cmd, row) {
  if (cmd === 'detail') openRow(row)
  else if (cmd === 'web') openWeb(row)
  else if (cmd === 'shell') openShell(row)
  else if (cmd === 'kick') kick(row)
}

const ctx = ref({ open: false, x: 0, y: 0, row: null })

function openContextMenu(row, column, event) {
  event.preventDefault()
  const w = 176
  const h = 250
  ctx.value = {
    open: true,
    row,
    x: Math.max(8, Math.min(event.clientX, window.innerWidth - w - 8)),
    y: Math.max(8, Math.min(event.clientY, window.innerHeight - h - 8)),
  }
}

function closeContextMenu() {
  ctx.value = { ...ctx.value, open: false }
}

function ctxAction(cmd) {
  const row = ctx.value.row
  closeContextMenu()
  if (row) onAction(cmd, row)
}

function openRow(row) {
  router.push('/agents/' + encodeURIComponent(row.devid))
}

function openShell(row) {
  router.push('/terminal/' + encodeURIComponent(row.devid))
}

const webDlg = ref(false)
const webRow = ref(null)
const webPort = ref('')
const webChips = ref([])
const webLoading = ref(false)
const validPort = computed(() => {
  const p = Number(webPort.value)
  return Number.isInteger(p) && p >= 1 && p <= 65535
})

// 字典里的端口 -> 名称，用于 chip 显示；无 dict.read 时静默跳过
async function portNames() {
  const names = new Map()
  if (!can('dict.read')) return names
  try {
    const d = await Api.get('/api/v1/permission/dicts?kind=ports&page=1&page_size=200')
    for (const it of d.items || []) {
      const v = String(it.value || '')
      const m = v.match(/^(\d+)(?:-(\d+))?$/)
      if (!m || !it.name) continue
      names.set([Number(m[1]), Number(m[2] || m[1])], it.name)
    }
  } catch (e) { /* 无权限或接口异常时只显示端口号 */ }
  return names
}

function nameOfPort(names, port) {
  for (const [range, name] of names) {
    if (port >= range[0] && port <= range[1]) return name
  }
  return ''
}

async function openWeb(row) {
  webRow.value = row
  webDlg.value = true
  webLoading.value = true
  webChips.value = []
  webPort.value = ''
  try {
    let ports = Array.isArray(row.caps_ports) ? row.caps_ports.slice() : null
    if (!ports) {
      // 老列表数据或刚上线：向设备要一次全量 sysinfo 拿 caps.ports
      try {
        const r = await Api.get('/api/v1/agents/' + encodeURIComponent(row.devid) + '/sysinfo?mode=full')
        const c = (r.sysinfo && r.sysinfo.caps) || {}
        ports = Array.isArray(c.ports) ? c.ports : []
      } catch (e) {
        ports = []
      }
    }
    const list = Array.from(new Set(ports.map(Number).filter((p) => p >= 1 && p <= 65535)))
    list.sort((a, b) => a - b)
    const names = await portNames()
    webChips.value = list.map((p) => {
      const name = nameOfPort(names, p)
      return { port: p, label: name || String(p), sub: name ? String(p) : '' }
    })
    const first = list.includes(80) ? 80 : list[0]
    if (!webPort.value) webPort.value = first ? String(first) : ''
  } finally {
    webLoading.value = false
  }
}

function confirmWeb() {
  if (!validPort.value || !webRow.value) return
  const url = '/device/' + encodeURIComponent(webRow.value.devid) + '/' +
    encodeURIComponent(webPort.value) + '/'
  webDlg.value = false
  window.open(url, '_blank')
}

async function kick(row) {
  try {
    await ElMessageBox.confirm(row.devid + ' 将下线并由 agent 退避重连。', '强制下线', {
      type: 'warning',
      confirmButtonText: '下线',
      cancelButtonText: '取消',
    })
  } catch (e) { return }
  try {
    await Api.post('/api/v1/agents/' + encodeURIComponent(row.devid) + '/kick')
    ElMessage.success('已下线 ' + row.devid)
    refresh(true)
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

let timer = null
let ro = null
function onKeydown(e) {
  if (e.key === 'Escape') closeContextMenu()
}

onMounted(() => {
  refresh()
  timer = setInterval(() => refresh(true), 5000)
  window.addEventListener('click', closeContextMenu)
  window.addEventListener('scroll', closeContextMenu, true)
  window.addEventListener('resize', closeContextMenu)
  window.addEventListener('keydown', onKeydown)
  if (tableWrap.value) {
    tableW.value = tableWrap.value.clientWidth
    if (typeof ResizeObserver !== 'undefined') {
      ro = new ResizeObserver((es) => { tableW.value = es[0].contentRect.width })
      ro.observe(tableWrap.value)
    }
  }
})
onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
  if (ro) ro.disconnect()
  window.removeEventListener('click', closeContextMenu)
  window.removeEventListener('scroll', closeContextMenu, true)
  window.removeEventListener('resize', closeContextMenu)
  window.removeEventListener('keydown', onKeydown)
})
</script>

<style scoped>
.search { width: 240px; }
:deep(.row-click) { cursor: pointer; }
.ctx-menu {
  position: fixed;
  z-index: 3000;
  min-width: 176px;
  padding: 6px;
  border-radius: 8px;
  background: var(--el-bg-color-overlay, #fff);
  border: 1px solid var(--el-border-color-light, #e4e7ed);
  box-shadow: 0 6px 24px rgba(0, 0, 0, 0.18);
}
.ctx-head {
  padding: 4px 10px 6px;
  margin-bottom: 4px;
  font-size: 12px;
  color: var(--el-text-color-secondary, #909399);
  border-bottom: 1px solid var(--el-border-color-lighter, #ebeef5);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.ctx-item {
  display: block;
  width: 100%;
  padding: 7px 10px;
  border: 0;
  border-radius: 6px;
  background: none;
  text-align: left;
  font-size: 13px;
  color: var(--el-text-color-regular, #606266);
  cursor: pointer;
}
.ctx-item:hover { background: var(--el-fill-color-light, #f5f7fa); }
.ctx-item.danger { color: var(--el-color-danger, #f56c6c); }
.ctx-sep { height: 1px; margin: 4px 6px; background: var(--el-border-color-lighter, #ebeef5); }
.web-pick { min-height: 96px; }
.web-tip { font-size: 12px; margin-bottom: 8px; }
.web-chips { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 12px; }
.web-chip {
  display: inline-flex; align-items: center; height: 26px; margin-right: 0; padding: 0 10px;
  border: 1px solid var(--el-border-color); border-radius: 13px;
  background: var(--el-fill-color-light);
}
.web-chip :deep(.el-radio__input) { display: none; }
.web-chip :deep(.el-radio__label) {
  display: inline-flex; align-items: center; gap: 4px; padding: 0;
  font-size: 12px; line-height: 1; color: var(--el-text-color-regular);
}
.web-chip:hover { border-color: var(--el-color-primary-light-5); }
.web-chip.is-checked {
  background: var(--el-color-primary-light-9); border-color: var(--el-color-primary-light-5);
}
.web-chip.is-checked :deep(.el-radio__label) { color: var(--el-color-primary); }
.web-label { max-width: 150px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.web-sub { font-size: 11px; opacity: .75; font-family: ui-monospace, Menlo, Consolas, monospace; }
.web-empty { font-size: 12px; margin-bottom: 8px; }
.web-input { margin-bottom: 8px; }
.web-note { font-size: 12px; line-height: 1.5; }
.web-note code { font-family: ui-monospace, Menlo, Consolas, monospace; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
  .web-label { max-width: 110px; }
}
</style>
