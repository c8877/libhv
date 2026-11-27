<template>
  <div class="set">
    <div class="set-head">
      <h2>设置</h2>
      <span class="muted">共 {{ schema.length }} 项 · 保存后立即生效</span>
    </div>

    <div class="set-wrap">
      <!-- 分组多、一屏放不下：左侧锚点跟着滚，右侧才是一组组的值 -->
      <el-anchor class="set-nav" :container="'.page'" direction="vertical" :offset="80">
        <el-anchor-link v-for="(g, i) in groups" :key="g.name" :href="'#set-' + i">
          {{ g.name }}
        </el-anchor-link>
      </el-anchor>

      <div class="set-body">
        <div v-for="(g, i) in groups" :id="'set-' + i" :key="g.name" class="panel">
          <div class="panel-head">
            <h2>{{ g.name }}</h2>
            <span class="spacer" />
            <el-button v-if="canSettingsWrite" @click="openEdit(g)">编辑</el-button>
          </div>
          <el-descriptions :column="isPhone ? 1 : 2" size="small" border>
            <el-descriptions-item v-for="d in g.defs" :key="d.key" :label="d.label">
              {{ valueOf(d) }}<span v-if="d.unit" class="unit">{{ d.unit }}</span>
              <div v-if="d.desc" class="muted">
                {{ d.desc }}<span v-if="d.min || d.max"> · 范围 {{ d.min }}–{{ d.max }}</span>
              </div>
            </el-descriptions-item>
          </el-descriptions>

          <div v-if="g.name === '告警' && canSettingsWrite" class="alert-bar">
            <span class="muted">{{ alertText }}</span>
            <span class="spacer" />
            <el-button size="small" :loading="testing" @click="sendTest">发送测试</el-button>
            <el-button size="small" @click="$router.push('/alerts')">看告警列表</el-button>
          </div>
        </div>
      </div>
    </div>

    <el-dialog v-model="dlg" :title="'编辑 ' + (group ? group.name : '设置')"
               :width="isPhone ? '94%' : '560px'">
      <el-form label-width="150px" class="edit-form">
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
      <div class="muted">只提交改动过的项；保存后立即生效。</div>
      <template #footer>
        <el-button @click="dlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="save">保存</el-button>
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

// g 为 null 时编辑全部（当前入口只按组编辑，保留这条路径给以后用）
function openEdit(g) {
  group.value = g
  Object.keys(form).forEach((k) => delete form[k])
  const defs = g ? g.defs : schema.value
  defs.forEach((d) => {
    const v = st.value[d.key]
    form[d.key] = isText(d) ? String(v == null ? '' : v) : (Number(v) || 0)
  })
  dlg.value = true
}

async function save() {
  saving.value = true
  try {
    const body = {}
    const defs = group.value ? group.value.defs : schema.value
    defs.forEach((d) => {
      const next = String(form[d.key])
      // 没改的项不提交：避免把未回显的密钥重写一遍
      if (next !== String(st.value[d.key] == null ? '' : st.value[d.key])) body[d.key] = next
    })
    if (!Object.keys(body).length) {
      dlg.value = false
      ElMessage.info('没有改动')
      return
    }
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
}

onMounted(load)
</script>

<style scoped>
.set-head { display: flex; align-items: baseline; gap: 10px; margin: 2px 2px 12px; }
.set-head h2 { margin: 0; font-size: 16px; font-weight: 650; }
.set-head .spacer { flex: 1; }
.set-wrap { display: grid; grid-template-columns: 132px minmax(0, 1fr); gap: 16px; align-items: start; }
.set-nav { position: sticky; top: 0; }
.set-nav :deep(.el-anchor__list) { padding: 0; }
.set-body { min-width: 0; }
.unit { margin-left: 6px; color: var(--el-text-color-secondary); font-size: 12px; }
.alert-bar { display: flex; align-items: center; gap: 8px; margin-top: 10px; flex-wrap: wrap; }
@media (max-width: 959px) {
  .set-wrap { grid-template-columns: 1fr; }
  .set-nav { display: none; }
}
</style>
