// 设备本地页入口：同一份 DeviceView 源码，直连 agent 的 /api/v1，不需要登录。
// 构建产物会被内联成单个 HTML，由 agent 内置提供，离线也能打开。
import { createApp, h } from 'vue'
import { RouterView, createRouter, createWebHashHistory } from 'vue-router'
import 'element-plus/theme-chalk/dark/css-vars.css'
// ElMessage / ElMessageBox 是脚本调用，模板里没有对应组件，必须手动引入样式
import 'element-plus/es/components/message/style/css'
import 'element-plus/es/components/message-box/style/css'
import '@/style.css'
import './local.css'
import DeviceView from '@/components/DeviceView.vue'
import LocalFallback from './LocalFallback.vue'
import { auth } from '@/shared/store.js'
import { installLocalApi } from './api.js'

// 本地页没有管理台账号体系：读权限全开，文件/服务按 agent 自身能力控制读写；
// 能力配置、采样配置、串口配置、电源、诊断、终端都只属于管理台。
const LOCAL_PERMS = [
  'device.view',
  'device.proc.read',
  'device.net.read',
  'device.metrics.read',
  'device.tty.read',
  'file.read',
  'file.write',
  'svc.read',
  'svc.write',
]

function applySystemTheme() {
  try {
    if (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) {
      document.documentElement.classList.add('dark')
    }
  } catch (e) { /* 忽略 */ }
}

async function bootDevid() {
  try {
    const r = await fetch('api/v1/sysinfo', { cache: 'no-store' })
    if (!r.ok) return ''
    const d = await r.json()
    return (d && d.devid) || ''
  } catch (e) {
    return ''
  }
}

async function main() {
  installLocalApi()
  auth.ready = true
  auth.me = { username: 'local', role: 'operator', super: false, perms: LOCAL_PERMS }
  applySystemTheme()

  const devid = await bootDevid()
  document.title = (devid || '设备') + ' · rtunnel 本地页'

  const router = createRouter({
    history: createWebHashHistory(),
    routes: [
      { path: '/', redirect: '/device/' + encodeURIComponent(devid || 'device') },
      {
        path: '/device/:devid',
        component: DeviceView,
        props: (route) => ({ devid: decodeURIComponent(route.params.devid || ''), mode: 'local' }),
      },
      { path: '/tty/:devid/:name', component: LocalFallback },
      { path: '/:pathMatch(.*)*', component: LocalFallback },
    ],
  })

  const App = { render: () => h('div', { class: 'local-shell' }, [h(RouterView)]) }
  const app = createApp(App)
  app.use(router)
  await router.replace('/device/' + encodeURIComponent(devid || 'device'))
  app.mount('#app')
}

main()
