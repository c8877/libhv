import { reactive } from 'vue'
import { Api } from './api.js'

export const auth = reactive({
  ready: false,
  setupDone: true,
  me: null,
  profile: {},
  settings: {},
})

export const overview = reactive({ data: null })

export const ROLE_NAMES = {
  admin: '超级管理员',
  manager: '管理员',
  operator: '操作员',
  viewer: '浏览者',
}

export function roleOf() {
  return (auth.me && auth.me.role) || 'viewer'
}

export function canOperate() {
  return ['admin', 'manager', 'operator'].includes(roleOf())
}

export function canManage() {
  return ['admin', 'manager'].includes(roleOf())
}

export function siteTitle() {
  return auth.settings['ui.site_title'] || 'rtunnel 管理台'
}

// ---------------------------------------------------------------- 主题

function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(hex || ''))
  if (!m) return null
  const n = parseInt(m[1], 16)
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255]
}

function mix(hex, base, weight) {
  const a = hexToRgb(hex)
  const b = hexToRgb(base)
  if (!a || !b) return hex
  const c = a.map((v, i) => Math.round(v * (1 - weight) + b[i] * weight))
  return '#' + c.map((v) => v.toString(16).padStart(2, '0')).join('')
}

export function applyTheme(mode, color) {
  mode = mode === 'dark' ? 'dark' : mode === 'color' ? 'color' : 'light'
  const dark = mode === 'dark'
  document.documentElement.classList.toggle('dark', dark)
  const accent = mode === 'color' && color ? color : '#3b82f6'
  const bg = dark ? '#141414' : '#ffffff'
  const root = document.documentElement.style
  root.setProperty('--el-color-primary', accent)
  for (const n of [3, 5, 7, 8, 9]) {
    root.setProperty('--el-color-primary-light-' + n, mix(accent, bg, n / 10))
  }
  root.setProperty('--el-color-primary-dark-2', mix(accent, '#000000', 0.2))
  try {
    localStorage.setItem('rt_theme', mode)
    localStorage.setItem('rt_theme_color', color || '')
  } catch (e) { /* 隐私模式 */ }
}

export function savedTheme() {
  try {
    const mode = localStorage.getItem('rt_theme')
    return {
      mode: mode === 'dark' || mode === 'color' ? mode : 'light',
      color: localStorage.getItem('rt_theme_color') || '',
    }
  } catch (e) {
    return { mode: 'light', color: '' }
  }
}

export async function saveTheme(patch, mode, color) {
  auth.profile = Object.assign({}, auth.profile, patch)
  applyTheme(mode, color)
  try {
    await Api.put('/api/v1/me/profile', patch)
  } catch (e) {
    // 保存失败不影响本地观感，下次进入仍用服务端值
  }
}

// ---------------------------------------------------------------- 启动

export async function ensureBoot() {
  if (auth.ready) return
  try {
    const st = await Api.get('/api/v1/setup/status')
    if (st && st.initialized === false) {
      auth.setupDone = false
      auth.ready = true
      return
    }
  } catch (e) {
    auth.setupDone = false
    auth.ready = true
    return
  }
  try {
    auth.me = await Api.get('/api/v1/me')
  } catch (e) {
    auth.me = null
  }
  if (auth.me) {
    try { auth.profile = await Api.get('/api/v1/me/profile') } catch (e) { auth.profile = {} }
    try { auth.settings = await Api.get('/api/v1/settings') } catch (e) { auth.settings = {} }
    if (auth.profile.display_name) auth.me.display_name = auth.profile.display_name
    if (auth.profile.avatar_color) auth.me.avatar_color = auth.profile.avatar_color
    const t = savedTheme()
    applyTheme(auth.profile.theme && auth.profile.theme !== 'system' ? auth.profile.theme : t.mode,
               auth.profile.theme_color || t.color)
  }
  auth.ready = true
}

export async function login(username, password) {
  await Api.post('/api/v1/login', { username, password })
  await reloadAuth()
}

export async function setup(token, username, password) {
  await Api.post('/api/v1/setup', { token, username, password })
  // setup 只建账号不签发会话，与旧前端一致，接着登录一次
  await Api.post('/api/v1/login', { username, password })
  await reloadAuth()
}

export async function reloadAuth() {
  auth.ready = false
  auth.setupDone = true
  await ensureBoot()
}

export async function logout() {
  try { await Api.post('/api/v1/logout') } catch (e) { /* 已失效 */ }
  auth.me = null
  auth.profile = {}
  try {
    localStorage.removeItem('rt_theme')
    localStorage.removeItem('rt_theme_color')
  } catch (e) { /* 忽略 */ }
}

export function avatarColor(name) {
  const palette = ['#4a9eff', '#57ab5a', '#f5a623', '#e5534b', '#b083f0', '#39c5cf', '#d4a72c', '#8b949e']
  let h = 0
  for (const ch of String(name || '?')) h = (h * 31 + ch.charCodeAt(0)) >>> 0
  return palette[h % palette.length]
}
