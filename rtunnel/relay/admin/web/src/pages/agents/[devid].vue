<template>
  <DeviceView :devid="devid" mode="admin" />
</template>

<script setup>
// 设备详情页的实现放在 components/DeviceView.vue，管理台与设备本地页共用同一份源码；
// 这里只负责从路由取 devid，后续挂载管理台的 API/权限适配。
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import DeviceView from '@/components/DeviceView.vue'

const route = useRoute()
const devid = computed(() => decodeURIComponent(route.params.devid || ''))
</script>
