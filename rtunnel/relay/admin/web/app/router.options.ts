import { createWebHashHistory } from 'vue-router'
import type { RouterConfig } from '@nuxt/schema'

// 沿用旧前端的 hash 路由：relay 静态服务只需返回 index.html 与 /_nuxt 产物，
// 不依赖 history 模式的 SPA fallback。
export default <RouterConfig>{
  history: (base) => createWebHashHistory(base),
}
