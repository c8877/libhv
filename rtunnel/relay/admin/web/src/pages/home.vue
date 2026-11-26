<template>
  <AiChat bind-query />
</template>

<script setup>
// 首页就是 AI 助手：不再放在仪表盘里，参考通用聊天页（居中对话 + 底部输入框）。
// 仪表盘保留在「概览」。
import AiChat from '@/components/AiChat.vue'
</script>
