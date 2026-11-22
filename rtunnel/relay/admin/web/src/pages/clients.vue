<template>
  <div class="panel">
    <div class="panel-head">
      <h2>客户端（A 侧）</h2>
      <span class="muted">共 {{ list.length }} 个</span>
      <span class="spacer" />
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>
    <DataTable :data="list" :sort-keys="sortKeys" v-loading="loading">
      <el-table-column label="用户" min-width="140">
        <template #default="{ row }">
          <b>{{ row.user }}</b>
          <div v-if="isPhone" class="muted">{{ row.remote_addr || '—' }}</div>
        </template>
      </el-table-column>
      <el-table-column v-if="!isPhone" prop="remote_addr" label="远端地址" min-width="150" />
      <el-table-column v-if="!isPhone" label="最近活跃" min-width="120">
        <template #default="{ row }">{{ ago(row.last_seen_ms) }} 前活跃</template>
      </el-table-column>
      <el-table-column prop="streams" label="会话数" width="80" align="right" />
      <el-table-column v-if="!isPhone" label="流量 ↓/↑" min-width="150">
        <template #default="{ row }">{{ bytes(row.bytes_in) }} / {{ bytes(row.bytes_out) }}</template>
      </el-table-column>
      <el-table-column label="操作" :width="isPhone ? 92 : 100">
        <template #default="{ row }">
          <el-button size="small" @click="openShell(row)">Shell</el-button>
        </template>
      </el-table-column>
      <template #empty><el-empty description="暂无在线客户端" :image-size="70" /></template>
    </DataTable>
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import DataTable from '@/components/DataTable.vue'
import { Api } from '@/shared/api.js'
import { ago, bytes } from '@/shared/utils.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const sortKeys = {
  '用户': 'user',
  '远端地址': 'remote_addr',
  '最近活跃': { key: 'last_seen_ms', type: 'num' },
  '会话数': { key: 'streams', type: 'num' },
  '流量 ↓/↑': { key: 'traffic', type: 'num', value: (r) => (r.bytes_in || 0) + (r.bytes_out || 0) }
}

const list = ref([])
const loading = ref(false)

async function load(silent) {
  if (!silent) loading.value = true
  try {
    list.value = await Api.get('/api/v1/clients')
  } catch (e) {
    if (!silent) ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function openShell(row) {
  router.push('/terminal/' + encodeURIComponent('a:' + row.user))
}

let timer = null
onMounted(() => {
  load()
  timer = setInterval(() => load(true), 5000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; }
}
</style>
