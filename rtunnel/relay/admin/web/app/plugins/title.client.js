import { titleFor } from '~/shared/pageTitle.js'

export default defineNuxtPlugin((nuxtApp) => {
  const router = nuxtApp.$router
  const apply = (to) => {
    const t = to.meta.title || titleFor(to.path)
    document.title = (t ? t + ' · ' : '') + 'rtunnel'
  }
  apply(router.currentRoute.value)
  router.afterEach(apply)
})
