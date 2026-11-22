import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import AutoImport from 'unplugin-auto-import/vite'
import Components from 'unplugin-vue-components/vite'
import { ElementPlusResolver } from 'unplugin-vue-components/resolvers'

// 设备本地页构建：和管理台共用 DeviceView 与 Element 组件，入口不同、
// API 由 src/local/api.js 适配。产物是单入口，最后会被脚本内联成单个 HTML
// 由 agent 内置提供。
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
  base: './',
  build: {
    outDir: '../../../agent/web/local-dist',
    emptyOutDir: true,
    assetsInlineLimit: 0,
    cssCodeSplit: false,
    rollupOptions: {
      input: { index: fileURLToPath(new URL('./local.html', import.meta.url)) },
      output: {
        inlineDynamicImports: true,
        entryFileNames: 'assets/local.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
})
