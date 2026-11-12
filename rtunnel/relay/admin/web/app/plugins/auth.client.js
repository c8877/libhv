import { auth } from '~/shared/store.js'
import { setUnauthorizedHandler } from '~/shared/api.js'

export default defineNuxtPlugin((nuxtApp) => {
  const router = nuxtApp.$router
  setUnauthorizedHandler(() => {
    auth.me = null
    if (router.currentRoute.value.path !== '/login') router.replace('/login')
  })
})
