import { auth, ensureBoot, roleOf } from '~/shared/store.js'
import { canVisit } from '~/shared/nav.js'

export default defineNuxtRouteMiddleware(async (to) => {
  await ensureBoot()

  if (to.path === '/acl' || to.path === '/groups') {
    return navigateTo('/permission', { replace: true })
  }

  const publicPage = to.path === '/login'
  if (!publicPage && !auth.me) return navigateTo('/login', { replace: true })
  if (publicPage && auth.me) return navigateTo('/overview', { replace: true })
  if (!publicPage && auth.me && !canVisit(to.path, roleOf())) {
    return navigateTo('/overview', { replace: true })
  }
})
