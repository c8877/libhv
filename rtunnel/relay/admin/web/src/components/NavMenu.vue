<template>
  <div class="nav-inner">
    <router-link class="logo" to="/" title="回到首页" aria-label="回到首页">
      <span class="mark">◈</span>
      <span class="name">{{ title }}</span>
    </router-link>
    <el-menu ref="menuRef" :default-active="active" :default-openeds="initialOpen"
             :unique-opened="true" router class="menu">
      <template v-for="g in items" :key="g.key">
        <!-- flat 组：不折叠，直接一个顶级入口（首页 AI 助手） -->
        <template v-if="g.flat">
          <el-menu-item v-for="n in g.items" :key="n.path" :index="n.path" class="nav-flat-item">
            <el-icon><component :is="iconOf(n.icon)" /></el-icon>
            <span>{{ n.label || titleOf(n.path) }}</span>
          </el-menu-item>
        </template>
        <el-sub-menu v-else :index="g.key">
          <template #title><span class="nav-group-title">{{ g.label }}</span></template>
          <el-menu-item v-for="n in g.items" :key="n.path" :index="n.path">
            <el-icon><component :is="iconOf(n.icon)" /></el-icon>
            <span>{{ n.label || titleOf(n.path) }}</span>
          </el-menu-item>
        </el-sub-menu>
      </template>
    </el-menu>
    <div class="side-foot">
      <template v-if="ov">
        <span class="dot" /> relay 在线 · {{ ov.agents }} 设备 · {{ ov.streams }} 会话
      </template>
      <template v-else>…</template>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { iconOf } from '@/shared/icons.js'

defineProps({
  items: { type: Array, default: () => [] },
  active: { type: String, default: '' },
  initialOpen: { type: Array, default: () => [] },
  title: { type: String, default: '' },
  ov: { type: Object, default: null },
})

const router = useRouter()
const menuRef = ref(null)
const titleOf = (path) => router.resolve(path).meta.title || path

defineExpose({
  openGroup(key) { if (key && menuRef.value) menuRef.value.open(key) },
})
</script>

<style scoped>
.nav-inner { display: flex; flex-direction: column; height: 100%; min-height: 0; }
.logo {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 20px 18px 18px;
  font-weight: 700;
  letter-spacing: 0.01em;
  flex: none;
  text-decoration: none;
  border-radius: 10px;
  transition: opacity 0.15s ease;
}
.logo:hover { opacity: 0.85; }
.logo:hover .mark { box-shadow: 0 8px 22px -8px var(--el-color-primary); }
.logo .mark {
  width: 30px;
  height: 30px;
  flex: none;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 9px;
  color: #fff;
  font-size: 16px;
  background: linear-gradient(135deg, var(--el-color-primary), color-mix(in srgb, var(--el-color-primary) 55%, #7c3aed));
  box-shadow: 0 8px 18px -10px var(--el-color-primary);
}
.logo .name { color: #f8fafc; font-size: 16px; }
.menu {
  --el-menu-bg-color: transparent;
  --el-menu-text-color: rgba(203, 213, 225, 0.78);
  --el-menu-hover-bg-color: rgba(148, 163, 184, 0.1);
  --el-menu-hover-text-color: #fff;
  --el-menu-active-color: #fff;
  --el-menu-item-height: 38px;
  border-right: 0;
  flex: 1;
  padding: 4px 0;
  overflow-y: auto;
}
.nav-flat-item { margin: 4px 8px 6px; border-radius: 8px; }
.menu :deep(.el-sub-menu__title) {
  height: 34px;
  line-height: 34px;
  margin: 2px 10px 0;
  padding: 0 10px !important;
  border-radius: 8px;
  font-size: 11px;
  letter-spacing: 0.06em;
  color: rgba(148, 163, 184, 0.55);
}
.menu :deep(.el-sub-menu:first-child > .el-sub-menu__title) {
  margin-top: 0;
}
.menu :deep(.el-sub-menu__title:hover) {
  color: #e2e8f0;
  background: rgba(148, 163, 184, 0.07);
}
.menu :deep(.el-sub-menu__icon-arrow) {
  right: 10px;
  margin-top: -5px;
  font-size: 11px;
}
.menu :deep(.el-sub-menu .el-menu-item) {
  padding-left: 30px !important;
}
.menu :deep(.el-menu-item) {
  position: relative;
  height: 38px;
  line-height: 38px;
  margin: 1px 10px;
  border-radius: 8px;
  font-size: 13.5px;
}
.menu :deep(.el-menu-item .el-icon) {
  margin-right: 10px;
  font-size: 16px;
  opacity: 0.9;
}
.menu :deep(.el-menu-item:hover) {
  color: #fff;
}
.menu :deep(.el-menu-item.is-active) {
  background: color-mix(in srgb, var(--el-color-primary) 26%, transparent);
  color: #fff;
}
.menu :deep(.el-menu-item.is-active)::before {
  content: "";
  position: absolute;
  left: 0;
  top: 11px;
  bottom: 11px;
  width: 3px;
  border-radius: 0 3px 3px 0;
  background: var(--el-color-primary);
}
.side-foot {
  padding: 12px 18px 14px;
  font-size: 12px;
  color: rgba(148, 163, 184, 0.7);
  border-top: 1px solid rgba(255, 255, 255, 0.07);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  flex: none;
}
.dot {
  display: inline-block;
  width: 6px;
  height: 6px;
  margin-right: 6px;
  border-radius: 50%;
  background: #34d399;
  box-shadow: 0 0 0 3px rgba(52, 211, 153, 0.15);
  vertical-align: 1px;
}
</style>
