<template>
  <div class="layout">
    <aside class="side">
      <NavMenu ref="deskNav" :items="items" :active="active" :initial-open="initialOpen"
               :title="title" :ov="ov" />
    </aside>

    <el-drawer v-model="navOpen" direction="ltr" :with-header="false" size="232px" class="nav-drawer">
      <NavMenu ref="drawerNav" :items="items" :active="active" :initial-open="initialOpen"
               :title="title" :ov="ov" />
    </el-drawer>

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
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import {
  auth, avatarColor, logout, roleOf, ROLE_NAMES, saveTheme, siteTitle, overview, can,
} from '@/shared/store.js'
import { navFor } from '@/shared/nav.js'
import NavMenu from '@/components/NavMenu.vue'
import { duration } from '@/shared/utils.js'

const route = useRoute()
const router = useRouter()
const navOpen = ref(false)
const themePop = ref(null)
const deskNav = ref(null)
const drawerNav = ref(null)
const ov = ref(overview.data)

const items = computed(() => navFor(roleOf(), can))
const active = computed(() => route.path)
const activeGroup = computed(() => {
  const p = route.path
  for (const g of items.value) {
    if (g.items.some((n) => p === n.path || p.startsWith(n.path + '/'))) return g.key
  }
  return ''
})
// 只在首次挂载时生效；之后靠 watch 调 menuRef.open()
const initialOpen = activeGroup.value ? [activeGroup.value] : []
const crumb = computed(() => route.meta.title || '')
const titleOf = (path) => router.resolve(path).meta.title || path
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

watch(() => route.path, async () => {
  navOpen.value = false
  const g = activeGroup.value
  if (!g) return
  await nextTick()
  if (deskNav.value) deskNav.value.openGroup(g)   // unique-opened 会自动收起其它组
})

watch(navOpen, async (v) => {
  if (!v) return
  await nextTick()
  if (drawerNav.value) drawerNav.value.openGroup(activeGroup.value)
})

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
.page { --page-pad: 24px; flex: 1; padding: var(--page-pad); min-width: 0; overflow-y: auto; }

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
  .side { display: none; }
  .hamburger { display: inline-flex; }
  .page { --page-pad: 16px; padding: var(--page-pad); }
}
@media (max-width: 599px) {
  .topbar { padding: 0 14px; gap: 8px; height: 56px; }
  .relay-info, .uname { display: none; }
  .page { --page-pad: 12px; padding: var(--page-pad); }
}
</style>

<style>
.nav-drawer .el-drawer__body {
  padding: 0;
  background: linear-gradient(180deg, #141b2b 0%, #0f1420 100%);
}
html.dark .nav-drawer .el-drawer__body {
  background: linear-gradient(180deg, #0b0f17 0%, #080b11 100%);
}
</style>
