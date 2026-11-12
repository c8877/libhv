<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>签发新证书</h2>
        <span class="spacer" />
        <el-button :loading="loading" @click="load">刷新列表</el-button>
      </div>
      <div class="issue-grid">
        <label>SAN<el-input v-model="issue.san" placeholder="PILE-000200" /></label>
        <label>类型
          <el-select v-model="issue.role">
            <el-option label="设备" value="device" />
            <el-option label="用户" value="user" />
          </el-select>
        </label>
        <label>有效期(天)<el-input-number v-model="issue.days" :min="1" :max="3650" /></label>
        <div class="issue-foot">
          <el-button type="primary" :loading="issuing" @click="doIssue">签发</el-button>
          <span v-if="issued" class="muted">
            已签发 {{ issued.san }}：
            <el-button link type="primary" @click="downloadPem(issued.san + '.crt', issued.cert)">下载 crt</el-button>
            <el-button link type="primary" @click="downloadPem(issued.san + '.key', issued.key)">下载 key（仅此一次）</el-button>
          </span>
        </div>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head"><h2>CSR 签名（私钥不出设备）</h2></div>
      <el-input v-model="csr" type="textarea" :rows="5" class="mono"
                placeholder="粘贴 CSR PEM（设备侧生成，SAN 或 CN 为身份）" />
      <div class="csr-foot">
        <span class="muted">有效期(天)</span>
        <el-input-number v-model="csrDays" :min="1" :max="3650" />
        <el-button type="primary" :loading="signing" @click="doSign">签名</el-button>
        <span v-if="signed" class="muted">
          已签发 {{ signed.san }}：
          <el-button link type="primary" @click="downloadPem(signed.san + '.crt', signed.cert)">下载 crt</el-button>
        </span>
      </div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>证书台账</h2>
        <span class="muted">共 {{ list.length }} 张</span>
        <span class="spacer" />
        <el-input v-model="q" placeholder="搜索 SAN" clearable class="search">
          <template #prefix><el-icon><Search /></el-icon></template>
        </el-input>
      </div>
      <el-table :data="filtered" v-loading="loading">
        <el-table-column label="SAN（身份）" :min-width="isPhone ? 150 : 200">
          <template #default="{ row }">
            <b :title="row.san">{{ row.san.length > 30 ? row.san.slice(0, 29) + '…' : row.san }}</b>
            <div v-if="isPhone" class="muted">
              {{ row.role === 'device' ? '设备' : row.role === 'user' ? '用户' : row.role }}
              · {{ expiry(row.expire_ms) }}
            </div>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="类型" width="80">
          <template #default="{ row }">{{ row.role === 'device' ? '设备' : row.role === 'user' ? '用户' : row.role }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="序列号" min-width="150">
          <template #default="{ row }">
            <code :title="serial(row.serial)">{{ serial(row.serial) }}</code>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="到期日" min-width="170">
          <template #default="{ row }">{{ expiry(row.expire_ms) }}</template>
        </el-table-column>
        <el-table-column label="状态" :width="isPhone ? 90 : 100">
          <template #default="{ row }">
            <el-tag :type="certType(row.status)" size="small">{{ certText(row.status) }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="90">
          <template #default="{ row }">
            <el-button v-if="row.status !== 'revoked'" size="small" type="danger" plain
                       @click="revoke(row)">吊销</el-button>
            <span v-else class="muted">—</span>
          </template>
        </el-table-column>
        <template #empty><el-empty description="暂无证书" :image-size="70" /></template>
      </el-table>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { Api } from '~/shared/api.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()

const list = ref([])
const q = ref('')
const loading = ref(false)
const issuing = ref(false)
const signing = ref(false)
const issued = ref(null)
const signed = ref(null)
const csr = ref('')
const csrDays = ref(30)
const issue = reactive({ san: '', role: 'device', days: 30 })

const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  return k ? list.value.filter((c) => String(c.san).toLowerCase().includes(k)) : list.value
})

function serial(n) {
  const s = '0x' + Number(n || 0).toString(16).toUpperCase()
  return s.length > 18 ? s.slice(0, 17) + '…' : s
}
function expiry(ms) {
  if (!ms) return '—'
  const d = new Date(ms)
  const days = Math.ceil((ms - Date.now()) / 86400000)
  const p = (x) => String(x).padStart(2, '0')
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' · ' +
    (days >= 0 ? '剩 ' + days + ' 天' : '已过期')
}
function certText(s) {
  return { valid: '有效', expiring: '即将到期', expired: '已过期', revoked: '已吊销' }[s] || s
}
function certType(s) {
  return { valid: 'success', expiring: 'warning', expired: 'danger', revoked: 'danger' }[s] || 'info'
}

async function load() {
  loading.value = true
  try {
    list.value = await Api.get('/api/v1/certs')
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function downloadPem(name, text) {
  const url = URL.createObjectURL(new Blob([text], { type: 'application/x-pem-file' }))
  const a = document.createElement('a')
  a.href = url
  a.download = name
  a.click()
  URL.revokeObjectURL(url)
}

async function doIssue() {
  const san = issue.san.trim()
  if (!san) {
    ElMessage.warning('请填写 SAN')
    return
  }
  issuing.value = true
  try {
    issued.value = await Api.post('/api/v1/certs/issue', {
      san, role: issue.role, days: Number(issue.days) || 30,
    })
    ElMessage.success('已签发 ' + issued.value.san)
    await load()
  } catch (e) {
    ElMessage.error('签发失败：' + e.message)
  } finally {
    issuing.value = false
  }
}

async function doSign() {
  if (!csr.value.trim()) {
    ElMessage.warning('请粘贴 CSR')
    return
  }
  signing.value = true
  try {
    signed.value = await Api.post('/api/v1/certs/sign', {
      csr: csr.value, days: Number(csrDays.value) || 30,
    })
    ElMessage.success('已签发 ' + signed.value.san)
    await load()
  } catch (e) {
    ElMessage.error('CSR 签名失败：' + e.message)
  } finally {
    signing.value = false
  }
}

async function revoke(row) {
  try {
    await ElMessageBox.confirm('吊销 ' + row.san + '？在线连接会被踢，后续 AUTH 被拒。', '吊销证书',
      { type: 'warning', confirmButtonText: '吊销', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.post('/api/v1/certs/revoke', { san: row.san })
    ElMessage.success('已吊销 ' + row.san)
    await load()
  } catch (e) {
    ElMessage.error('吊销失败：' + e.message)
  }
}

onMounted(load)
</script>

<style scoped>
.issue-grid {
  display: grid;
  grid-template-columns: minmax(0, 2fr) 160px 150px;
  gap: 12px;
  align-items: end;
}
.issue-grid label { display: flex; flex-direction: column; gap: 6px; font-size: 12px; color: var(--el-text-color-secondary); }
.issue-foot { grid-column: 1 / -1; display: flex; align-items: center; gap: 10px; }
.csr-foot { display: flex; align-items: center; gap: 10px; margin-top: 10px; }
.search { width: 220px; }
@media (max-width: 900px) {
  .issue-grid { grid-template-columns: 1fr 1fr; }
}
@media (max-width: 599px) {
  .issue-grid { grid-template-columns: 1fr; }
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
  .csr-foot { flex-wrap: wrap; }
}
</style>
