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

    <el-table :data="filtered" v-loading="loading" row-class-name="row-click"
              @row-click="openRow" @row-contextmenu="openContextMenu">
      <el-table-column label="devid" min-width="140">
        <template #default="{ row }"><b>{{ row.devid }}</b></template>
      </el-table-column>
      <el-table-column label="状态" width="90">
        <template #default><el-tag type="success" size="small">在线</el-tag></template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150" />
      <el-table-column v-if="!isPhone" label="最近活跃" min-width="120">
        <template #default="{ row }">{{ ago(row.last_seen_ms) }} 前活跃</template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="系统运行" min-width="110">
        <template #default="{ row }">
          <span v-if="row.boot_ms">{{ duration(nowMs - row.boot_ms) }}</span>
          <span v-else class="muted">—</span>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="在线时长" min-width="110">
        <template #default="{ row }">
          <span v-if="row.connected_at_ms">{{ duration(nowMs - row.connected_at_ms) }}</span>
          <span v-else class="muted">—</span>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="streams" label="会话数" width="90" align="right" />
      <el-table-column v-if="!isPhone" label="流量 ↓/↑" min-width="150">
        <template #default="{ row }">{{ bytes(row.bytes_in) }} / {{ bytes(row.bytes_out) }}</template>
      </el-table-column>
      <el-table-column :min-width="isPhone ? 64 : 72" label="操作" align="center">
        <template #default="{ row }">
          <span @click.stop>
            <el-dropdown trigger="click" @command="(c) => onAction(c, row)">
              <el-button size="small" text class="more-btn" aria-label="更多操作">
                <el-icon><MoreFilled /></el-icon>
              </el-button>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="detail">详情</el-dropdown-item>
                  <el-dropdown-item v-if="canOperate()" command="web" divided>Web</el-dropdown-item>
                  <el-dropdown-item v-if="canOperate() && row.caps_shell !== 0" command="shell">Shell</el-dropdown-item>
                  <el-dropdown-item v-if="canManage()" command="bind" divided>绑定</el-dropdown-item>
                  <el-dropdown-item v-if="canManage()" command="kick">下线</el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
          </span>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无在线设备" :image-size="70" /></template>
    </el-table>

    <teleport to="body">
      <div v-if="ctx.open" class="ctx-menu" :style="{ left: ctx.x + 'px', top: ctx.y + 'px' }"
           @contextmenu.prevent @mousedown.stop>
        <div class="ctx-head">{{ ctx.row && ctx.row.devid }}</div>
        <button class="ctx-item" @click="ctxAction('detail')">详情</button>
        <button v-if="canOperate()" class="ctx-item" @click="ctxAction('web')">打开 Web</button>
        <button v-if="canOperate() && ctx.row && ctx.row.caps_shell !== 0" class="ctx-item"
                @click="ctxAction('shell')">打开 Shell</button>
        <div v-if="canManage()" class="ctx-sep" />
        <button v-if="canManage()" class="ctx-item" @click="ctxAction('bind')">绑定账号</button>
        <button v-if="canManage()" class="ctx-item danger" @click="ctxAction('kick')">强制下线</button>
      </div>
    </teleport>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { canManage, canOperate } from '@/shared/store.js'
import { ago, bytes, duration } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const list = ref([])
const q = ref('')
const loading = ref(false)
const nowMs = ref(Date.now())

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
  else if (cmd === 'bind') bind(row)
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

async function openWeb(row) {
  let port
  try {
    const r = await ElMessageBox.prompt('端口（需在 agent --allow 列表）', '设备 Web 端口', {
      inputValue: '80',
      confirmButtonText: '打开',
      cancelButtonText: '取消',
    })
    port = r.value
  } catch (e) { return }
  if (!port) return
  window.open('/device/' + encodeURIComponent(row.devid) + '/' + encodeURIComponent(port) + '/', '_blank')
}

async function bind(row) {
  let owner
  try {
    const r = await ElMessageBox.prompt('账号（留空 = 解绑）', '绑定设备 ' + row.devid, {
      inputValue: row.owner || '',
      confirmButtonText: '保存',
      cancelButtonText: '取消',
    })
    owner = r.value
  } catch (e) { return }
  try {
    await Api.put('/api/v1/devices/' + encodeURIComponent(row.devid) + '/owner', { username: owner })
    ElMessage.success(owner ? '已绑定到 ' + owner : '已解绑')
    refresh(true)
  } catch (e) {
    ElMessage.error('绑定失败：' + e.message)
  }
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
})
onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
  window.removeEventListener('click', closeContextMenu)
  window.removeEventListener('scroll', closeContextMenu, true)
  window.removeEventListener('resize', closeContextMenu)
  window.removeEventListener('keydown', onKeydown)
})
</script>

<style scoped>
.search { width: 240px; }
:deep(.row-click) { cursor: pointer; }
.more-btn { padding: 4px 6px; }
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
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
