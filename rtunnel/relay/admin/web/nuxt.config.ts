export default defineNuxtConfig({
  ssr: false,
  devtools: { enabled: false },
  compatibilityDate: '2026-11-01',
  modules: ['@element-plus/nuxt'],
  elementPlus: {
    importStyle: 'css',
    themes: ['dark'],
    installMethods: ['ElMessage', 'ElMessageBox', 'ElNotification', 'ElLoading'],
  },
  css: [
    '~/assets/css/main.css',
  ],
  app: {
    head: {
      title: 'rtunnel',
      meta: [
        { charset: 'utf-8' },
        { name: 'viewport', content: 'width=device-width, initial-scale=1' },
      ],
    },
  },
  vite: {
    build: {
      chunkSizeWarningLimit: 2000,
    },
  },
})
