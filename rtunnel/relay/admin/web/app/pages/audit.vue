<template>
  <div class="panel">
    <div class="panel-head">
      <h2>审计日志</h2>
      <span class="muted">最近 {{ rows.length }} 条（新到旧）</span>
      <span class="spacer" />
      <el-input v-model="q" placeholder="搜索操作 / 详情" clearable class="search">
        <template #prefix><el-icon><Search /></el-icon></template>
      </el-input>
      <el-button :loading="loading" @click="load">刷新</el-button>
    </div>
    <el-table :data="filtered" v-loading="loading" size="small">
      <template v-if="isPhone">
        <el-table-column label="操作 / 详情">
          <template #default="{ row }">
            <code>{{ row.action }}</code>
            <div class="detail">{{ row.detail }}</div>
            <div class="muted">{{ row.at }} · {{ row.role }}</div>
          </template>
        </el-table-column>
      </template>
      <template v-else>
        <el-table-column label="时间" width="170">
          <template #default="{ row }"><span class="muted">{{ row.at }}</span></template>
        </el-table-column>
        <el-table-column prop="role" label="角色" width="100" />
        <el-table-column label="操作" min-width="180">
          <template #default="{ row }"><code>{{ row.action }}</code></template>
        </el-table-column>
        <el-table-column label="详情" min-width="260">
          <template #default="{ row }">
            <span :title="row.detail">{{ row.detail.length > 100 ? row.detail.slice(0, 99) + '…' : row.detail }}</span>
          </template>
        </el-table-column>
      </template>
      <template #empty><el-empty description="暂无审计记录" :image-size="70" /></template>
    </el-table>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { Api } from '~/shared/api.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()

const rows = ref([])
const q = ref('')
const loading = ref(false)

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  if (!k) return rows.value
  return rows.value.filter((x) =>
    x.action.toLowerCase().includes(k) || x.detail.toLowerCase().includes(k))
})

function parse(line) {
  const m = /^\[([^\]]+)\] role=(\S+) action=(\S+) detail=(.*)$/.exec(line)
  if (!m) return { at: '', role: '', action: '', detail: line }
  return {
    at: m[1].replace('T', ' ').replace('Z', ''),
    role: m[2], action: m[3], detail: m[4],
  }
}

async function load() {
  loading.value = true
  try {
    const arr = await Api.get('/api/v1/audit')
    rows.value = (arr || []).map(parse)
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.search { width: 240px; }
.detail { margin: 4px 0; word-break: break-all; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
