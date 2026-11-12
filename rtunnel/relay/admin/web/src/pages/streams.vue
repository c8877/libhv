<template>
  <div class="panel">
    <div class="panel-head">
      <h2>会话</h2>
      <span class="muted">共 {{ list.length }} 条</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索用户 / 设备" clearable class="search">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
    </div>
    <el-table :data="filtered" v-loading="loading">
      <el-table-column label="A 用户:a_sid" :min-width="isPhone ? 150 : 160">
        <template #default="{ row }">
          {{ row.a_user }}:{{ row.a_sid }}
          <div v-if="isPhone" class="muted">→ {{ row.c_devid }}:{{ row.c_sid }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="C 设备:c_sid" min-width="160">
        <template #default="{ row }">{{ row.c_devid }}:{{ row.c_sid }}</template>
      </el-table-column>
      <el-table-column label="状态" :width="isPhone ? 90 : 120">
        <template #default="{ row }">
          <el-tag :type="stateType(row.state)" size="small">{{ row.state }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" label="开启于" min-width="120">
        <template #default="{ row }">{{ ago(row.opened_at_ms) }} 前</template>
      </el-table-column>
      <el-table-column label="操作" width="90">
        <template #default="{ row }">
          <el-button size="small" type="danger" plain @click="kick(row)">踢掉</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="当前无会话" :image-size="70" /></template>
    </el-table>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { ago } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const list = ref([])
const q = ref('')
const loading = ref(false)

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return list.value
  return list.value.filter((s) =>
    String(s.a_user).toLowerCase().includes(k) ||
    String(s.c_devid || '').toLowerCase().includes(k))
})

function stateType(s) {
  return { OPEN: 'success', OPENING: 'primary', HALF_CLOSED: 'warning' }[s] || 'warning'
}

async function load(silent) {
  if (!silent) loading.value = true
  try {
    list.value = await Api.get('/api/v1/streams')
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

async function kick(row) {
  try {
    await ElMessageBox.confirm('a_sid=' + row.a_sid + ' 将被双侧 RESET。', '踢掉会话',
      { type: 'warning', confirmButtonText: '踢掉', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/streams/' + row.a_sid)
    ElMessage.success('已踢掉 ' + row.a_sid)
    await load(true)
  } catch (e) {
    ElMessage.error('操作失败：' + e.message)
  }
}

let timer = null
onMounted(() => {
  load()
  timer = setInterval(() => load(true), 5000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.search { width: 240px; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
