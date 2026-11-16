import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import AutoImport from 'unplugin-auto-import/vite'
import Components from 'unplugin-vue-components/vite'
import { ElementPlusResolver } from 'unplugin-vue-components/resolvers'

export default defineConfig({
  plugins: [
    vue(),
    AutoImport({
      resolvers: [ElementPlusResolver()],
      dts: false,
    }),
    Components({
      resolvers: [ElementPlusResolver()],
      dts: false,
    }),
  ],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  // fs-ui.js 在 rtunnel/shared/web 下，与本地 Web 共用；dev 模式需要放行仓库目录
  server: {
    fs: {
      allow: [fileURLToPath(new URL('../../..', import.meta.url))],
    },
  },
  build: {
    chunkSizeWarningLimit: 2000,
  },
})
