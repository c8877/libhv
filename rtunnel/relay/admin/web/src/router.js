import { createRouter, createWebHashHistory } from 'vue-router'
import { auth, ensureBoot, roleOf } from './shared/store.js'
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
  { path: '/permission', component: () => import('./pages/permission.vue'), meta: { title: '权限' } },
  { path: '/clients', component: () => import('./pages/clients.vue'), meta: { title: '客户端 A' } },
  { path: '/access', component: () => import('./pages/access/index.vue'), meta: { title: '接入账号' } },
  { path: '/access/:id', component: () => import('./pages/access/[id].vue'), meta: { title: '接入账号详情' } },
  { path: '/streams', component: () => import('./pages/streams.vue'), meta: { title: '会话' } },
  { path: '/certs', component: () => import('./pages/certs.vue'), meta: { title: '证书' } },
  { path: '/accounts', component: () => import('./pages/accounts.vue'), meta: { title: '账号' } },
  { path: '/files', component: () => import('./pages/files.vue'), meta: { title: '文件' } },
  { path: '/distribute', component: () => import('./pages/distribute.vue'), meta: { title: '分发文件' } },
  { path: '/tasks', component: () => import('./pages/tasks/index.vue'), meta: { title: '任务' } },
  { path: '/tasks/:id', component: () => import('./pages/tasks/[id].vue'), meta: { title: '任务详情' } },
  { path: '/audit', component: () => import('./pages/audit.vue'), meta: { title: '审计' } },
  { path: '/settings', component: () => import('./pages/settings.vue'), meta: { title: '设置' } },
  { path: '/profile', component: () => import('./pages/profile.vue'), meta: { title: '个人设置' } },
  // 旧前端遗留入口
  { path: '/acl', redirect: '/permission' },
  { path: '/groups', redirect: '/permission' },
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
  if (!to.meta.public && auth.me && !canVisit(to.path, roleOf())) return { path: '/overview', replace: true }
  document.title = (to.meta.title ? to.meta.title + ' · ' : '') + 'rtunnel'
  return true
})

setUnauthorizedHandler(() => {
  auth.me = null
  if (location.hash !== '#/login') router.replace('/login')
})

export default router
