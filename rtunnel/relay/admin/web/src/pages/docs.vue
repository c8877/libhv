<template>
  <div class="panel">
    <div class="panel-head">
      <h2>接口文档</h2>
      <span class="muted">共 {{ total }} 个接口（能否调用由权限组决定）</span>
      <span class="spacer" />
      <el-button v-if="can('token.read')" @click="$router.push('/tokens')">
        API Token
      </el-button>
    </div>
    <div class="tip muted">
      所有 HTTP 接口都支持 <code>Authorization: Bearer atk-…</code>；
      能否调用取决于账号/令牌所在权限组勾了哪些资源键，以及组的设备范围。
      这里列出全部接口，不做角色裁剪；实际返回 403 时去「权限 → 权限组」补资源键。
    </div>
    <ApiDocPanel />
  </div>
</template>

<script setup>
import { computed } from 'vue'
import ApiDocPanel from '@/components/ApiDocPanel.vue'
import { can } from '@/shared/store.js'
import { DOCS } from '@/shared/api-docs.js'

const total = computed(() => DOCS.length)
</script>

<style scoped>
.tip { margin-bottom: 12px; font-size: 12.5px; line-height: 1.7; }
.tip code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
@media (max-width: 599px) {
  .panel-head { flex-wrap: wrap; row-gap: 6px; }
  .panel-head h2 { white-space: nowrap; }
}
</style>
