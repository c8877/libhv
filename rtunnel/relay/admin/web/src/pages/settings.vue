<template>
  <div>
    <div v-for="g in groups" :key="g.name" class="panel">
      <div class="panel-head">
        <h2>{{ g.name }}</h2>
        <span class="spacer" />
        <el-button @click="openEdit(g)">编辑</el-button>
      </div>
      <el-descriptions :column="isPhone ? 1 : 2" size="small" border>
        <el-descriptions-item v-for="d in g.defs" :key="d.key" :label="d.label">
          {{ valueOf(d) }}<span v-if="d.unit" class="unit">{{ d.unit }}</span>
          <div v-if="d.desc" class="muted">{{ d.desc }}<span v-if="d.min || d.max"> · 范围 {{ d.min }}–{{ d.max }}</span></div>
        </el-descriptions-item>
      </el-descriptions>
      <div v-if="g.name === '告警' && canSettingsWrite" class="alert-bar">
        <span class="muted">{{ alertText }}</span>
        <span class="spacer" />
        <el-button size="small" :loading="testing" @click="sendTest">发送测试</el-button>
      </div>
      <div v-if="g.name === 'AI 模型'" class="key-bar">
        <div class="key-state">
          <el-tag size="small" :type="aiCfg && aiCfg.key_set ? 'success' : 'info'">
            {{ aiCfg && aiCfg.key_set ? '密钥已配置' : '密钥未配置' }}
          </el-tag>
          <span class="muted">{{ aiKeyHint }}</span>
        </div>
        <template v-if="canSettingsWrite">
          <el-input v-model="newKey" type="password" show-password placeholder="sk-…" class="key-input"
                    maxlength="512" @keyup.enter="saveKey" />
          <el-button :loading="keySaving" @click="saveKey">保存密钥</el-button>
          <el-button v-if="aiCfg && aiCfg.key_set" :loading="keySaving" @click="clearKey">清除</el-button>
        </template>
        <span v-else class="muted">需要 settings.write 才能修改</span>
      </div>
    </div>

    <el-dialog v-model="dlg" :title="'编辑 ' + (group ? group.name : '')" width="520px">
      <el-form label-width="150px">
        <el-form-item v-for="d in (group ? group.defs : [])" :key="d.key" :label="d.label">
          <el-input v-if="isText(d)" v-model="form[d.key]"
                    :type="d.secret ? 'password' : 'text'" :show-password="!!d.secret"
                    :maxlength="Number(d.maxlen) || 64" />
          <template v-else>
            <el-input-number v-model="form[d.key]" :min="Number(d.min)" :max="Number(d.max)" />
            <span v-if="d.unit" class="unit">{{ d.unit }}</span>
          </template>
        </el-form-item>
      </el-form>
      <div class="muted">保存后立即生效。</div>
      <template #footer>
        <el-button @click="dlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="save">保存本组</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { Api } from '@/shared/api.js'
import { auth, can } from '@/shared/store.js'
import { useBreakpoint } from '@/shared/useBreakpoint.js'

const { isPhone } = useBreakpoint()
const canSettingsWrite = computed(() => can('settings.write'))
const st = ref(auth.settings || {})
const schema = computed(() => st.value._schema || [])
const groups = computed(() => {
  const out = []
  schema.value.forEach((d) => {
    let g = out.find((x) => x.name === d.group)
    if (!g) {
      g = { name: d.group, defs: [] }
      out.push(g)
    }
    g.defs.push(d)
  })
  return out
})

const dlg = ref(false)
const saving = ref(false)
const group = ref(null)
const form = reactive({})
const alertStatus = ref(null)
const testing = ref(false)
const alertText = computed(() => {
  const s = alertStatus.value
  if (!s) return '告警状态加载中…'
  if (!s.enabled) return '告警未启用'
  let t = s.sent_total ? ('已发送 ' + s.sent_total + ' 条') : '尚未发送'
  if (s.queue) t += '，队列 ' + s.queue
  if (s.devices_offline) t += '，离线设备 ' + s.devices_offline
  if (s.last_error) t += ' · 最近错误：' + s.last_error
  return t
})


function isText(d) {
  return Number(d.min) === 0 && Number(d.max) === 0
}
function valueOf(d) {
  const v = st.value[d.key]
  if (d.secret) return v ? '已配置' : '—'
  return v === undefined || v === '' ? '—' : v
}

function openEdit(g) {
  group.value = g
  Object.keys(form).forEach((k) => delete form[k])
  g.defs.forEach((d) => {
    const v = st.value[d.key]
    form[d.key] = isText(d) ? String(v == null ? '' : v) : (Number(v) || 0)
  })
  dlg.value = true
}

async function save() {
  saving.value = true
  try {
    const body = {}
    group.value.defs.forEach((d) => { body[d.key] = String(form[d.key]) })
    await Api.put('/api/v1/settings', body)
    st.value = await Api.get('/api/v1/settings')
    auth.settings = st.value
    ElMessage.success('已保存，立即生效')
    dlg.value = false
    loadAlertStatus()
    } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function loadAlertStatus() {
  try {
    alertStatus.value = await Api.get('/api/v1/alerts/status')
  } catch (e) {
    alertStatus.value = null
  }
}

async function sendTest() {
  testing.value = true
  try {
    await Api.post('/api/v1/alerts/test', {})
    ElMessage.success('测试告警已入队，请查看通知渠道')
    setTimeout(loadAlertStatus, 1500)
  } catch (e) {
    ElMessage.error('发送失败：' + e.message)
  } finally {
    testing.value = false
  }
}

async function load() {
  try {
    st.value = await Api.get('/api/v1/settings')
    auth.settings = st.value
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  }
  loadAlertStatus()
  loadAi()
}

// ---------------------------------------------------------------- AI 密钥
// 密钥不在 settings 表里：单独的 GET/PUT/DELETE /api/v1/ai/key，
// 响应只回 key_set 与文件路径，永远不回内容。
const aiCfg = ref(null)
const newKey = ref('')
const keySaving = ref(false)
const aiKeyHint = computed(() => {
  const c = aiCfg.value
  if (!c) return '状态未知'
  if (!c.key_set) return '保存后才能使用「AI 助手」'
  const t = c.key_updated_ms ? new Date(c.key_updated_ms).toLocaleString() : ''
  return (c.key_path || '') + (t ? ' · ' + t : '')
})

async function loadAi() {
  if (!can('ai.chat')) return
  try {
    aiCfg.value = await Api.get('/api/v1/ai/config')
  } catch (e) {
    aiCfg.value = null
  }
}

async function saveKey() {
  const k = newKey.value.trim()
  if (!k) {
    ElMessage.warning('请输入密钥')
    return
  }
  keySaving.value = true
  try {
    aiCfg.value = await Api.put('/api/v1/ai/key', { key: k })
    newKey.value = ''
    ElMessage.success('密钥已保存（仅写入本机文件，不回显）')
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    keySaving.value = false
  }
}

async function clearKey() {
  keySaving.value = true
  try {
    aiCfg.value = await Api.del('/api/v1/ai/key')
    ElMessage.success('密钥已清除')
  } catch (e) {
    ElMessage.error('清除失败：' + e.message)
  } finally {
    keySaving.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.unit { margin-left: 6px; color: var(--el-text-color-secondary); font-size: 12px; }
.alert-bar { display: flex; align-items: center; gap: 8px; margin-top: 10px; }
.key-bar { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; margin-top: 10px; }
.key-state { display: flex; align-items: center; gap: 8px; }
.key-input { width: 280px; }
</style>
