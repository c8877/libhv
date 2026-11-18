import {
  ArrowDown, Avatar, Collection, Connection, Document, Fold, FolderOpened, Key, List, Loading, Lock,
  Monitor, Moon, Odometer, Postcard, Refresh, Search, Setting, Sort, Sunny, Tickets, Upload,
  User, UserFilled,
} from '@element-plus/icons-vue'

// 全局注册的图标；新增菜单图标时在这里补一项（nav.js 只写名字）
export const ICONS = {
  ArrowDown, Avatar, Collection, Connection, Document, Fold, FolderOpened, Key, List, Loading, Lock,
  Monitor, Moon, Odometer, Postcard, Refresh, Search, Setting, Sort, Sunny, Tickets, Upload,
  User, UserFilled,
}

const warned = new Set()

// 未注册的图标名退回 Document，并在开发环境提示一次，避免文字顶到侧栏最左
export function iconOf(name) {
  if (ICONS[name]) return name
  if (import.meta.env.DEV && name && !warned.has(name)) {
    warned.add(name)
    console.warn('[icon] not registered: ' + name + '，已退回 Document')
  }
  return 'Document'
}
