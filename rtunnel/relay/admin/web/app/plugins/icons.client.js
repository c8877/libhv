// 仅注册模板与导航实际用到的图标，避免把 @element-plus/icons-vue 全量打进包
import {
  ArrowDown, Connection, Document, Fold, FolderOpened, Key, List, Lock,
  Monitor, Moon, Odometer, Search, Setting, Sort, Sunny, Tickets, User,
} from '@element-plus/icons-vue'

const ICONS = {
  ArrowDown, Connection, Document, Fold, FolderOpened, Key, List, Lock,
  Monitor, Moon, Odometer, Search, Setting, Sort, Sunny, Tickets, User,
}

export default defineNuxtPlugin((nuxtApp) => {
  for (const [name, comp] of Object.entries(ICONS)) {
    nuxtApp.vueApp.component(name, comp)
  }
})
