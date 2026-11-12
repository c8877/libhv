<template>
  <div>
    <div class="cards">
      <div v-for="s in stats" :key="s.label" class="stat">
        <div class="num">{{ s.value }}</div>
        <div class="label">{{ s.label }}</div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>运行信息</h2>
        <span class="spacer" />
        <span class="muted">5s 自动刷新</span>
      </div>
      <el-descriptions v-if="o" :column="2" border>
        <el-descriptions-item label="版本">{{ o.version }}</el-descriptions-item>
        <el-descriptions-item label="运行时长">{{ duration(o.uptime_ms) }}</el-descriptions-item>
        <el-descriptions-item label="mTLS">
          <el-tag :type="o.mtls ? 'success' : 'warning'" size="small">{{ o.mtls ? '开启' : '关闭' }}</el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="ACL">
          <el-tag :type="o.allow_all ? 'warning' : 'primary'" size="small">
            {{ o.allow_all ? 'allow-all' : '按表授权' }}
          </el-tag>
        </el-descriptions-item>
      </el-descriptions>
      <el-skeleton v-else :rows="2" animated />
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { overview } from '@/shared/store.js'
import { bytes, duration } from '@/shared/utils.js'

const o = ref(overview.data)
const stats = computed(() => {
  const d = o.value || {}
  return [
    { label: '在线设备', value: d.agents ?? '—' },
    { label: '在线客户端', value: d.clients ?? '—' },
    { label: '活跃会话', value: d.streams ?? '—' },
    { label: '累计下行', value: d.bytes_in != null ? bytes(d.bytes_in) : '—' },
    { label: '累计上行', value: d.bytes_out != null ? bytes(d.bytes_out) : '—' },
  ]
})

let timer = null
async function pull() {
  try {
    overview.data = await Api.get('/api/v1/overview')
    o.value = overview.data
  } catch (e) { /* 保留上次数据 */ }
}

onMounted(() => {
  pull()
  timer = setInterval(pull, 5000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  gap: 14px;
  margin-bottom: 18px;
}
.stat {
  position: relative;
  overflow: hidden;
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color-lighter);
  border-radius: var(--app-radius);
  box-shadow: var(--app-shadow);
  padding: 18px 18px 16px;
  transition: transform 0.18s ease, box-shadow 0.18s ease;
}
.stat::before {
  content: "";
  position: absolute;
  left: 0;
  top: 0;
  width: 30px;
  height: 3px;
  border-radius: 0 0 4px 0;
  background: var(--el-color-primary);
  opacity: 0.85;
}
.stat:hover {
  transform: translateY(-1px);
  box-shadow: 0 2px 4px rgba(15, 23, 42, 0.05), 0 20px 44px -26px rgba(15, 23, 42, 0.4);
}
.num {
  font-size: 28px;
  font-weight: 700;
  line-height: 1.15;
  letter-spacing: -0.02em;
  font-variant-numeric: tabular-nums;
}
.label {
  margin-top: 6px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}
</style>
