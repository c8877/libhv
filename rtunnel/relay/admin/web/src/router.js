import { createRouter, createWebHashHistory } from 'vue-router'
import { auth, ensureBoot, roleOf, can } from './shared/store.js'
import { canVisit } from './shared/nav.js'
import { setUnauthorizedHandler } from './shared/api.js'

const routes = [
  { path: '/login', component: () => import('./pages/login.vue'), meta: { public: true, title: '登录' } },
  { path: '/', redirect: '/overview' },
  { path: '/overview', component: () => import('./pages/overview.vue'), meta: { title: '概览' } },
  { path: '/agents', component: () => import('./pages/agents/index.vue'), meta: { title: '设备 C' } },
  { path: '/agents/:devid', component: () => import('./pages/agents/[devid].vue'), meta: { title: '设备详情' } },
  { path: '/terminal/:devid', component: () => import('./pages/terminal/[devid].vue'), meta: { title: '终端' } },
  { path: '/tty/:devid/:alias', component: () => import('./pages/tty/console.vue'), meta: { title: '串口调试' } },
  { path: '/permission', redirect: '/groups' },
  { path: '/groups', component: () => import('./pages/perm-groups.vue'), meta: { title: '权限组' } },
  { path: '/subjects', component: () => import('./pages/subjects.vue'), meta: { title: '授权对象' } },
  { path: '/dicts', component: () => import('./pages/dicts.vue'), meta: { title: '字典库' } },
  { path: '/clients', component: () => import('./pages/clients.vue'), meta: { title: '客户端 A' } },
  { path: '/access', component: () => import('./pages/access/index.vue'), meta: { title: '接入账号' } },
  { path: '/access/:id', component: () => import('./pages/access/[id].vue'), meta: { title: '接入账号详情' } },
  { path: '/tokens', component: () => import('./pages/tokens.vue'), meta: { title: 'API Token' } },
  { path: '/docs', component: () => import('./pages/docs.vue'), meta: { title: '接口文档' } },
  { path: '/streams', component: () => import('./pages/streams.vue'), meta: { title: '会话' } },
  { path: '/certs', component: () => import('./pages/certs.vue'), meta: { title: '证书' } },
  { path: '/accounts', component: () => import('./pages/accounts.vue'), meta: { title: '账号' } },
  { path: '/files', component: () => import('./pages/files.vue'), meta: { title: '文件' } },
  { path: '/distribute', component: () => import('./pages/distribute.vue'), meta: { title: '分发文件' } },
  { path: '/tasks', component: () => import('./pages/tasks/index.vue'), meta: { title: '任务' } },
  { path: '/tasks/:id', component: () => import('./pages/tasks/[id].vue'), meta: { title: '任务详情' } },
  { path: '/ota', component: () => import('./pages/ota.vue'), meta: { title: 'Agent 升级' } },
  { path: '/relay-list', component: () => import('./pages/relay-list.vue'), meta: { title: '中继列表' } },
  { path: '/usage', component: () => import('./pages/usage.vue'), meta: { title: '用量' } },
  { path: '/tenants', component: () => import('./pages/tenants.vue'), meta: { title: '租户' } },
  { path: '/audit', component: () => import('./pages/audit.vue'), meta: { title: '审计' } },
  { path: '/settings', component: () => import('./pages/settings.vue'), meta: { title: '设置' } },
  { path: '/profile', component: () => import('./pages/profile.vue'), meta: { title: '个人设置' } },
  { path: '/:pathMatch(.*)*', redirect: '/overview' },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
})

router.beforeEach(async (to) => {
  await ensureBoot()
  if (!to.meta.public && !auth.me) return { path: '/login', replace: true }
  if (to.path === '/login' && auth.me) return { path: '/', replace: true }
  if (!to.meta.public && auth.me && !canVisit(to.path, roleOf(), can)) return { path: '/overview', replace: true }
  document.title = (to.meta.title ? to.meta.title + ' · ' : '') + 'rtunnel'
  return true
})

setUnauthorizedHandler(() => {
  auth.me = null
  if (location.hash !== '#/login') router.replace('/login')
})

// 管理台重新部署后旧 chunk 文件名已不存在，停留在旧页面的标签页点菜单会加载失败。
// 这里整页重载一次拿新入口；用 sessionStorage 标记防止持续失败时反复刷新。
router.onError((err) => {
  const msg = String((err && err.message) || err)
  if (!/dynamically imported module|Importing a module script failed|error loading dynamically imported module/i.test(msg)) return
  if (sessionStorage.getItem('chunk-reload')) return
  sessionStorage.setItem('chunk-reload', '1')
  location.reload()
})

router.afterEach(() => {
  sessionStorage.removeItem('chunk-reload')
})

export default router
