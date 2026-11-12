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
        <slot />
      </main>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Api } from '~/shared/api.js'
import {
  auth, avatarColor, logout, roleOf, ROLE_NAMES, saveTheme, siteTitle, overview,
} from '~/shared/store.js'
import { navFor } from '~/shared/nav.js'
import { duration } from '~/shared/utils.js'
import { titleFor } from '~/shared/pageTitle.js'

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
  min-height: 100vh;
  background: var(--el-fill-color-light);
}
.side {
  width: 216px;
  flex: none;
  display: flex;
  flex-direction: column;
  background: var(--el-bg-color);
  border-right: 1px solid var(--el-border-color-light);
}
.logo {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 18px 20px 14px;
  font-weight: 700;
  letter-spacing: 0.02em;
}
.logo .mark { color: var(--el-color-primary); font-size: 16px; }
.logo .name { font-size: 15px; }
.menu { border-right: 0; flex: 1; }
.menu :deep(.el-menu-item) { height: 42px; line-height: 42px; margin: 2px 8px; border-radius: 6px; }
.menu :deep(.el-menu-item.is-active) { background: var(--el-color-primary-light-9); }
.side-foot {
  padding: 12px 20px 16px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
  border-top: 1px solid var(--el-border-color-lighter);
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
  background: var(--el-color-success);
  vertical-align: 1px;
}
.main { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.topbar {
  display: flex;
  align-items: center;
  gap: 10px;
  height: 56px;
  padding: 0 20px;
  background: var(--el-bg-color);
  border-bottom: 1px solid var(--el-border-color-light);
}
.hamburger { display: none; }
.crumb { font-size: 15px; font-weight: 600; }
.spacer { flex: 1; }
.relay-info { font-size: 12px; color: var(--el-text-color-secondary); white-space: nowrap; }
.icon-btn { color: var(--el-text-color-regular); }
.user { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; outline: none; }
.avatar {
  width: 28px;
  height: 28px;
  border-radius: 50%;
  color: #fff;
  font-size: 12px;
  font-weight: 600;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.uname { font-size: 13px; max-width: 140px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.caret { color: var(--el-text-color-secondary); }
.who { display: flex; flex-direction: column; line-height: 1.5; }
.who span { font-size: 12px; color: var(--el-text-color-secondary); }
.page { flex: 1; padding: 20px; min-width: 0; }
.nav-mask { display: none; }

.theme-box { display: flex; flex-direction: column; gap: 12px; }
.theme-modes { display: flex; gap: 6px; }
.theme-modes .el-button { flex: 1; }
.swatches { display: flex; gap: 8px; }
.swatches i {
  width: 22px;
  height: 22px;
  border-radius: 50%;
  cursor: pointer;
  border: 2px solid transparent;
}
.swatches i.on { border-color: var(--el-text-color-primary); }
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
  .topbar { padding: 0 12px; gap: 8px; height: 52px; }
  .relay-info, .uname { display: none; }
  .page { padding: 12px; }
}
</style>
