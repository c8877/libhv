<template>
  <div class="panel local-fallback">
    <h2>本地页面不提供这个入口</h2>
    <p class="muted">串口调试和交互终端需要经 relay 管理台打开。</p>
    <el-button type="primary" @click="back">返回设备页</el-button>
  </div>
</template>

<script setup>
import { useRoute, useRouter } from 'vue-router'

const route = useRoute()
const router = useRouter()

function back() {
  router.push('/device/' + encodeURIComponent(route.params.devid || 'device'))
}
</script>

<style scoped>
.local-fallback {
  max-width: 560px;
  margin: 48px auto;
  text-align: center;
}
.local-fallback h2 {
  margin: 0 0 8px;
  font-size: 16px;
}
.local-fallback p {
  margin: 0 0 16px;
}
</style>
