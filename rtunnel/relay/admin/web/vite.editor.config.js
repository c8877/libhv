import { defineConfig } from 'vite'

// 把 CodeMirror 打成单文件 IIFE：window.hvCreateEditor
// 产物 rtunnel/shared/web/cm-editor.bundle.js 同时供：
//   - relay-admin：作为静态资源引入（带 hash，可被 immutable 缓存）
//   - agent 本地页：CMake 内嵌到二进制，从 /assets/cm-editor.js 提供
export default defineConfig({
  build: {
    lib: {
      entry: 'src/editor/cm-editor-entry.js',
      formats: ['iife'],
      name: 'HvCmEditor',
      fileName: () => 'cm-editor.bundle.js'
    },
    outDir: '../../../shared/web',
    emptyOutDir: false,
    minify: true,
    target: 'es2019',
    sourcemap: false
  }
})
