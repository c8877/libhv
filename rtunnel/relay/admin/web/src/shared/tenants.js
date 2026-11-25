// 租户下拉与名称展示：接入账号、设备字典、用量、费用页共用。
// 没有 tenant.read 时保持空列表，页面照常工作（只显示默认租户）。
import { ref } from 'vue'
import { Api } from './api.js'
import { can } from './store.js'

export function useTenants() {
  const tenants = ref([])

  async function loadTenants() {
    if (!can('tenant.read')) return
    const res = await Api.get('/api/v1/tenants').catch(() => null)
    tenants.value = (res && res.items) || []
  }

  function tenantName(id) {
    const t = tenants.value.find((x) => x.id === id)
    return t ? t.name : (id === 1 ? '默认租户' : '—')
  }

  return { tenants, loadTenants, tenantName }
}
