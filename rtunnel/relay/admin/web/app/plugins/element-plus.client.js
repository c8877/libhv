import ElementPlus from 'element-plus'
import * as Icons from '@element-plus/icons-vue'

export default defineNuxtPlugin((nuxtApp) => {
  nuxtApp.vueApp.use(ElementPlus)
  for (const [name, comp] of Object.entries(Icons)) {
    nuxtApp.vueApp.component(name, comp)
  }
})
