import { createApp } from 'vue'
import 'element-plus/theme-chalk/dark/css-vars.css'
import {
  ArrowDown, Connection, Document, Fold, FolderOpened, Key, List, Loading, Lock,
  Monitor, Moon, Odometer, Search, Setting, Sort, Sunny, Tickets, User,
} from '@element-plus/icons-vue'
import App from './App.vue'
import router from './router.js'
import './style.css'

const ICONS = {
  ArrowDown, Connection, Document, Fold, FolderOpened, Key, List, Loading, Lock,
  Monitor, Moon, Odometer, Search, Setting, Sort, Sunny, Tickets, User,
}

const app = createApp(App)
for (const [name, comp] of Object.entries(ICONS)) {
  app.component(name, comp)
}
app.use(router)
app.mount('#app')
