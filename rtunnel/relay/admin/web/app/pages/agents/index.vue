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

    <el-table :data="filtered" v-loading="loading" row-class-name="row-click" @row-click="openRow">
      <el-table-column label="devid" min-width="140">
        <template #default="{ row }"><b>{{ row.devid }}</b></template>
      </el-table-column>
      <el-table-column label="状态" width="90">
        <template #default><el-tag type="success" size="small">在线</el-tag></template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="分组" min-width="140">
        <template #default="{ row }">
          <template v-if="groups[row.devid] && groups[row.devid].length">
            <el-tag v-for="g in groups[row.devid]" :key="g" size="small" class="group">{{ g }}</el-tag>
          </template>
          <span v-else class="muted">—</span>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150" />
      <el-table-column v-if="!isPhone" label="最近活跃" min-width="120">
        <template #default="{ row }">{{ ago(row.last_seen_ms) }} 前活跃</template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="streams" label="会话数" width="90" align="right" />
      <el-table-column v-if="!isPhone" label="流量 ↓/↑" min-width="150">
        <template #default="{ row }">{{ bytes(row.bytes_in) }} / {{ bytes(row.bytes_out) }}</template>
      </el-table-column>
      <el-table-column :min-width="isPhone ? 92 : 250" label="操作">
        <template #default="{ row }">
          <el-dropdown v-if="isPhone" trigger="click" @command="(c) => onAction(c, row)">
            <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
            <template #dropdown>
              <el-dropdown-menu>
                <el-dropdown-item v-if="canOperate()" command="web">Web</el-dropdown-item>
                <el-dropdown-item v-if="canOperate() && row.caps_shell !== 0" command="shell">Shell</el-dropdown-item>
                <el-dropdown-item v-if="canManage()" command="bind" divided>绑定</el-dropdown-item>
                <el-dropdown-item v-if="canManage()" command="kick">下线</el-dropdown-item>
              </el-dropdown-menu>
            </template>
          </el-dropdown>
          <template v-else>
            <el-button v-if="canOperate()" size="small" @click.stop="openWeb(row)">Web</el-button>
            <el-button v-if="canOperate() && row.caps_shell !== 0" size="small" @click.stop="openShell(row)">Shell</el-button>
            <el-button v-if="canManage()" size="small" @click.stop="bind(row)">绑定</el-button>
            <el-button v-if="canManage()" size="small" type="danger" plain @click.stop="kick(row)">下线</el-button>
          </template>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无在线设备" :image-size="70" /></template>
    </el-table>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '~/shared/api.js'
import { canManage, canOperate } from '~/shared/store.js'
import { ago, bytes } from '~/shared/utils.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const list = ref([])
const groups = ref({})
const q = ref('')
const loading = ref(false)

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return list.value
  return list.value.filter((a) =>
    String(a.devid).toLowerCase().includes(k) ||
    String(a.remote_addr || '').toLowerCase().includes(k))
})

async function refresh(silent) {
  if (!silent) loading.value = true
  try {
    list.value = await Api.get('/api/v1/agents')
    try { groups.value = await Api.get('/api/v1/groups/device-map') } catch (e) { groups.value = {} }
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onAction(cmd, row) {
  if (cmd === 'web') openWeb(row)
  else if (cmd === 'shell') openShell(row)
  else if (cmd === 'bind') bind(row)
  else if (cmd === 'kick') kick(row)
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
onMounted(() => {
  refresh()
  timer = setInterval(() => refresh(true), 5000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.search { width: 240px; }
.group { margin-right: 4px; }
:deep(.row-click) { cursor: pointer; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
