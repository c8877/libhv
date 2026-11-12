<template>
  <div class="layout">
    <aside class="side" :class="{ open: navOpen }">
      <div class="logo">
        <span class="mark">◈</span>
        <span class="name">{{ title }}</span>
      </div>
      <el-menu :default-active="active" router class="menu">
        <el-menu-item v-for="n in items" :key="n.path" :index="n.path">
          <el-icon><component :is="n.icon" /></el-icon>
          <span>{{ n.title }}</span>
        </el-menu-item>
      </el-menu>
      <div class="side-foot">
        <template v-if="ov">
          <span class="dot" /> relay 在线 · {{ ov.agents }} 设备 · {{ ov.streams }} 会话
        </template>
        <template v-else>…</template>
      </div>
    </aside>

    <div v-if="navOpen" class="nav-mask" @click="navOpen = false" />

    <div class="main">
      <header class="topbar">
        <el-button text class="hamburger" aria-label="菜单" @click="navOpen = !navOpen">
          <el-icon :size="18"><Fold /></el-icon>
        </el-button>
        <span class="crumb">{{ crumb }}</span>
        <span class="spacer" />
        <span v-if="ov" class="relay-info">v{{ ov.version }} · uptime {{ duration(ov.uptime_ms) }}</span>

        <el-popover ref="themePop" placement="bottom-end" :width="248" trigger="click">
          <template #reference>
            <el-button circle text class="icon-btn" aria-label="主题">
              <el-icon :size="17"><Sunny v-if="mode !== 'dark'" /><Moon v-else /></el-icon>
            </el-button>
          </template>
          <div class="theme-box">
            <div class="theme-modes">
              <el-button v-for="m in modes" :key="m.key" size="small"
                         :type="mode === m.key ? 'primary' : 'default'"
                         :plain="mode !== m.key" @click="setMode(m.key)">{{ m.label }}</el-button>
            </div>
            <div class="swatches">
              <i v-for="c in swatches" :key="c" :style="{ background: c }"
                 :class="{ on: mode === 'color' && color === c }" @click="setColor(c)" />
            </div>
            <div class="custom">
              <span>自定义</span>
              <el-color-picker v-model="color" size="small" @change="setColor" />
            </div>
          </div>
        </el-popover>

        <el-dropdown trigger="click" @command="onUserCommand">
          <span class="user">
            <span class="avatar" :style="{ background: avatarColor(name) }">{{ initial }}</span>
            <span class="uname">{{ name }}</span>
            <el-icon class="caret"><ArrowDown /></el-icon>
          </span>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item disabled>
                <div class="who">
                  <b>{{ name }}</b>
                  <span>{{ roleName }}</span>
                </div>
              </el-dropdown-item>
              <el-dropdown-item command="profile" divided>个人设置</el-dropdown-item>
              <el-dropdown-item command="logout">退出登录</el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </header>

      <main class="page">
        <router-view />
      </main>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import {
  auth, avatarColor, logout, roleOf, ROLE_NAMES, saveTheme, siteTitle, overview,
} from '@/shared/store.js'
import { navFor } from '@/shared/nav.js'
import { duration } from '@/shared/utils.js'
import { titleFor } from '@/shared/pageTitle.js'

const route = useRoute()
const router = useRouter()
const navOpen = ref(false)
const themePop = ref(null)
const ov = ref(overview.data)

const items = computed(() => navFor(roleOf()))
const active = computed(() => route.path)
const crumb = computed(() => route.meta.title || titleFor(route.path))
const title = computed(() => siteTitle())
const name = computed(() => (auth.me && (auth.me.display_name || auth.me.username)) || '—')
const initial = computed(() => String(name.value).slice(0, 1).toUpperCase())
const roleName = computed(() => ROLE_NAMES[roleOf()] || roleOf())

const modes = [
  { key: 'light', label: '浅色' },
  { key: 'dark', label: '深色' },
  { key: 'color', label: '纯色' },
]
const swatches = ['#3b82f6', '#57ab5a', '#f5a623', '#e5534b', '#b083f0', '#39c5cf', '#d4a72c']
const mode = ref('light')
const color = ref('#3b82f6')

watch(() => route.path, () => { navOpen.value = false })

function syncTheme() {
  const p = auth.profile || {}
  mode.value = p.theme === 'dark' || p.theme === 'color' ? p.theme : 'light'
  color.value = p.theme_color || '#3b82f6'
}

function setMode(m) {
  mode.value = m
  saveTheme({ theme: m, theme_color: color.value }, m, m === 'color' ? color.value : '')
  themePop.value && themePop.value.hide()
}

function setColor(c) {
  if (!c) return
  color.value = c
  mode.value = 'color'
  saveTheme({ theme: 'color', theme_color: c }, 'color', c)
  themePop.value && themePop.value.hide()
}

async function onUserCommand(cmd) {
  if (cmd === 'profile') router.push('/profile')
  if (cmd === 'logout') {
    await logout()
    ElMessage.success('已退出登录')
    router.replace('/login')
  }
}

let timer = null
async function pullOverview() {
  try {
    overview.data = await Api.get('/api/v1/overview')
    ov.value = overview.data
  } catch (e) { /* 概览失败不影响导航 */ }
}

onMounted(() => {
  syncTheme()
  pullOverview()
  timer = setInterval(pullOverview, 15000)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<style scoped>
.layout {
  display: flex;
  height: 100vh;
  overflow: hidden;
  background: var(--el-bg-color-page);
}
.side {
  width: var(--app-nav-width);
  flex: none;
  display: flex;
  flex-direction: column;
  background: linear-gradient(180deg, #141b2b 0%, #0f1420 100%);
  border-right: 1px solid rgba(255, 255, 255, 0.05);
}
html.dark .side {
  background: linear-gradient(180deg, #0b0f17 0%, #080b11 100%);
}
.logo {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 20px 18px 18px;
  font-weight: 700;
  letter-spacing: 0.01em;
}
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
  --el-menu-item-height: 40px;
  border-right: 0;
  flex: 1;
  padding: 4px 0;
  overflow-y: auto;
}
.menu :deep(.el-menu-item) {
  position: relative;
  height: 40px;
  line-height: 40px;
  margin: 2px 10px;
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
.main { flex: 1; min-width: 0; display: flex; flex-direction: column; height: 100vh; }
.topbar {
  display: flex;
  align-items: center;
  gap: 10px;
  height: 64px;
  flex: none;
  padding: 0 24px;
  background: var(--el-bg-color);
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.hamburger { display: none; }
.crumb { font-size: 16px; font-weight: 650; letter-spacing: 0.01em; }
.spacer { flex: 1; }
.relay-info {
  padding: 3px 10px;
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 999px;
  background: var(--el-fill-color-light);
  font-size: 11.5px;
  color: var(--el-text-color-secondary);
  white-space: nowrap;
  font-variant-numeric: tabular-nums;
}
.icon-btn { color: var(--el-text-color-regular); }
.user { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; outline: none; }
.avatar {
  width: 30px;
  height: 30px;
  border-radius: 50%;
  color: #fff;
  font-size: 12px;
  font-weight: 600;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 0 0 3px var(--el-fill-color-light);
}
.uname { font-size: 13px; max-width: 140px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.caret { color: var(--el-text-color-secondary); }
.who { display: flex; flex-direction: column; line-height: 1.5; }
.who span { font-size: 12px; color: var(--el-text-color-secondary); }
.page { flex: 1; padding: 24px; min-width: 0; overflow-y: auto; }
.nav-mask { display: none; }

.theme-box { display: flex; flex-direction: column; gap: 12px; }
.theme-modes {
  display: flex;
  gap: 4px;
  padding: 3px;
  border-radius: 9px;
  background: var(--el-fill-color-light);
}
.theme-modes .el-button { flex: 1; }
.swatches { display: flex; gap: 9px; }
.swatches i {
  width: 22px;
  height: 22px;
  border-radius: 50%;
  cursor: pointer;
  border: 2px solid transparent;
  box-shadow: 0 0 0 1px var(--el-border-color-darker) inset;
}
.swatches i.on {
  border-color: var(--el-bg-color);
  box-shadow: 0 0 0 2px var(--el-color-primary);
}
.custom { display: flex; align-items: center; justify-content: space-between; font-size: 12px; color: var(--el-text-color-secondary); }

@media (max-width: 959px) {
  .side {
    position: fixed;
    left: 0;
    top: 0;
    bottom: 0;
    z-index: 2200;
    transform: translateX(-100%);
    transition: transform 0.22s ease;
    box-shadow: 8px 0 28px rgba(0, 0, 0, 0.18);
  }
  .side.open { transform: none; }
  .nav-mask {
    display: block;
    position: fixed;
    inset: 0;
    z-index: 2100;
    background: rgba(0, 0, 0, 0.45);
  }
  .hamburger { display: inline-flex; }
  .page { padding: 16px; }
}
@media (max-width: 599px) {
  .topbar { padding: 0 14px; gap: 8px; height: 56px; }
  .relay-info, .uname { display: none; }
  .page { padding: 12px; }
}
</style>
