<template>
  <div class="chat-page">
    <AiChat bind-query />
  </div>
</template>

<script setup>
// 对话本身只有一份实现（components/AiChat.vue）：这里是全屏页，
// 首页仪表盘用 compact 模式嵌入同一个组件。
import AiChat from '@/components/AiChat.vue'
</script>

<style scoped>
.chat-page { display: flex; justify-content: center; }
.chat-page :deep(.ai-chat) { width: 100%; max-width: 980px; }
</style>
