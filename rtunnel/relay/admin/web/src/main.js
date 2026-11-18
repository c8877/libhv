import { createApp } from 'vue'
import 'element-plus/theme-chalk/dark/css-vars.css'
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
