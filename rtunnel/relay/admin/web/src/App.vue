<template>
  <el-config-provider :locale="zhCn">
    <router-view v-if="isLogin" />
    <AppLayout v-else-if="auth.ready" />
    <div v-else class="boot">
      <el-icon class="is-loading" :size="22"><Loading /></el-icon>
    </div>
  </el-config-provider>
</template>

<script setup>
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import { auth } from '@/shared/store.js'
import AppLayout from './layout/AppLayout.vue'

const route = useRoute()
const isLogin = computed(() => route.path === '/login')
</script>

<style scoped>
.boot {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--el-text-color-secondary);
}
</style>
