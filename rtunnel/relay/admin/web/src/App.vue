<template>
  <router-view v-if="isLogin" />
  <AppLayout v-else-if="auth.ready" />
  <div v-else class="boot">
    <el-icon class="is-loading" :size="22"><Loading /></el-icon>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useRoute } from 'vue-router'
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
