<template>
  <div class="panel">
    <div class="panel-head">
      <h2>接口文档</h2>
      <span class="muted">当前角色 {{ role }} · 可调用 {{ total }} 个接口</span>
      <span class="spacer" />
      <el-button v-if="canManage()" @click="$router.push('/tokens')">
        API Token
      </el-button>
    </div>
    <div class="tip muted">
      所有 HTTP 接口都支持 <code>Authorization: Bearer atk-…</code>；
      Token 继承归属账号的角色，绑定设备分组后只会出现在组内设备上。
      文档按角色裁剪，取消「只显示我可调用」可查看更高权限接口。
    </div>
    <ApiDocPanel :role="role" />
  </div>
</template>

<script setup>
import { computed } from 'vue'
import ApiDocPanel from '@/components/ApiDocPanel.vue'
import { canManage, roleOf } from '@/shared/store.js'
import { docsForRole } from '@/shared/api-docs.js'

const role = computed(() => roleOf())
const total = computed(() => docsForRole(role.value).length)
</script>

<style scoped>
.tip { margin-bottom: 12px; font-size: 12.5px; line-height: 1.7; }
.tip code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; row-gap: 6px; }
  .panel-head h2 { white-space: nowrap; }
}
</style>
