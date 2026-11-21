import { createApp } from 'vue'
import 'element-plus/theme-chalk/dark/css-vars.css'
// ElMessage / ElMessageBox 是脚本调用，模板里没有对应组件，unplugin 的
// ElementPlusResolver 不会为显式 import 补样式，必须手动引入，否则弹窗无样式
import 'element-plus/es/components/message/style/css'
import 'element-plus/es/components/message-box/style/css'
import App from './App.vue'
import router from './router.js'
import { ICONS } from './shared/icons.js'
import './style.css'

const app = createApp(App)
for (const [name, comp] of Object.entries(ICONS)) {
  app.component(name, comp)
}
app.use(router)
app.mount('#app')
