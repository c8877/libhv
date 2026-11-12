<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>个人设置</h2>
        <span class="spacer" />
        <el-button @click="openProfile">编辑资料</el-button>
      </div>
      <el-descriptions :column="isPhone ? 1 : 2" size="small" border>
        <el-descriptions-item label="头像颜色">
          <span class="color-dot" :style="{ background: profile.avatar_color || '#4a9eff' }" />
          <code>{{ profile.avatar_color || '#4a9eff' }}</code>
        </el-descriptions-item>
        <el-descriptions-item label="显示名">{{ profile.display_name || '—' }}</el-descriptions-item>
        <el-descriptions-item label="默认页面">
          {{ profile.default_page === 'agents' ? '设备列表' : '概览' }}
        </el-descriptions-item>
        <el-descriptions-item label="时间格式">
          {{ profile.time_format === 'relative' ? '相对时间' : '本地时间' }}
        </el-descriptions-item>
        <el-descriptions-item label="终端字号">{{ profile.term_font || 14 }} px</el-descriptions-item>
      </el-descriptions>
    </div>

    <div class="panel">
      <div class="panel-head"><h2>安全</h2></div>
      <div class="sec-row">
        <div>
          <b>登录密码</b>
          <div class="muted">修改当前账号密码，其他会话会失效。</div>
        </div>
        <el-button @click="openPassword">修改密码</el-button>
      </div>
      <div class="sec-row">
        <div>
          <b>登录会话</b>
          <div class="muted">吊销本账号全部登录会话（含当前）。</div>
        </div>
        <el-button type="danger" plain @click="logoutAll">登出所有</el-button>
      </div>
    </div>

    <el-dialog v-model="pfDlg" title="编辑资料" width="460px">
      <el-form :model="pf" label-width="88px">
        <el-form-item label="头像颜色">
          <el-color-picker v-model="pf.avatar_color" />
        </el-form-item>
        <el-form-item label="显示名">
          <el-input v-model="pf.display_name" maxlength="64" />
        </el-form-item>
        <el-form-item label="默认页面">
          <el-select v-model="pf.default_page" style="width: 100%">
            <el-option label="概览" value="overview" />
            <el-option label="设备列表" value="agents" />
          </el-select>
        </el-form-item>
        <el-form-item label="时间格式">
          <el-select v-model="pf.time_format" style="width: 100%">
            <el-option label="本地时间" value="local" />
            <el-option label="相对时间" value="relative" />
          </el-select>
        </el-form-item>
        <el-form-item label="终端字号">
          <el-input-number v-model="pf.term_font" :min="10" :max="24" /> <span class="unit">px</span>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="pfDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveProfile">保存</el-button>
      </template>
    </el-dialog>

    <el-dialog v-model="pwDlg" title="修改密码" width="420px">
      <el-form :model="pw" label-width="80px">
        <el-form-item label="当前密码">
          <el-input v-model="pw.old" type="password" show-password autocomplete="current-password" />
        </el-form-item>
        <el-form-item label="新密码">
          <el-input v-model="pw.new" type="password" show-password autocomplete="new-password"
                    placeholder="至少 8 位" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="pwDlg = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="savePassword">修改密码</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '~/shared/api.js'
import { auth, logout } from '~/shared/store.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const profile = ref(auth.profile || {})
const saving = ref(false)
const pfDlg = ref(false)
const pwDlg = ref(false)
const pf = reactive({ avatar_color: '#4a9eff', display_name: '', default_page: 'overview', time_format: 'local', term_font: 14 })
const pw = reactive({ old: '', new: '' })

async function load() {
  try {
    profile.value = await Api.get('/api/v1/me/profile')
    auth.profile = profile.value
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  }
}

function openProfile() {
  Object.assign(pf, {
    avatar_color: profile.value.avatar_color || '#4a9eff',
    display_name: profile.value.display_name || '',
    default_page: profile.value.default_page || 'overview',
    time_format: profile.value.time_format || 'local',
    term_font: Number(profile.value.term_font) || 14,
  })
  pfDlg.value = true
}

async function saveProfile() {
  saving.value = true
  try {
    const p = await Api.put('/api/v1/me/profile', {
      avatar_color: pf.avatar_color,
      display_name: pf.display_name,
      default_page: pf.default_page,
      time_format: pf.time_format,
      term_font: String(pf.term_font),
    })
    profile.value = p || Object.assign({}, profile.value, pf)
    auth.profile = profile.value
    if (auth.me) {
      auth.me.display_name = pf.display_name
      auth.me.avatar_color = pf.avatar_color
    }
    ElMessage.success('已保存')
    pfDlg.value = false
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    saving.value = false
  }
}

function openPassword() {
  pw.old = ''
  pw.new = ''
  pwDlg.value = true
}

async function savePassword() {
  if (pw.new.length < 8) {
    ElMessage.warning('新密码至少 8 位')
    return
  }
  saving.value = true
  try {
    await Api.put('/api/v1/users/' + auth.me.id + '/password', {
      old_password: pw.old,
      new_password: pw.new,
    })
    ElMessage.success('密码已修改')
    pwDlg.value = false
  } catch (e) {
    ElMessage.error('修改失败：' + e.message)
  } finally {
    saving.value = false
  }
}

async function logoutAll() {
  try {
    await ElMessageBox.confirm('本账号全部登录会话（含当前）将被吊销。', '登出所有会话',
      { type: 'warning', confirmButtonText: '登出', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.post('/api/v1/me/logout-all', {})
    await logout()
    router.replace('/login')
  } catch (e) {
    ElMessage.error('失败：' + e.message)
  }
}

onMounted(load)
</script>

<style scoped>
.color-dot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; vertical-align: -1px; margin-right: 6px; }
.sec-row { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 12px 0; border-bottom: 1px solid var(--el-border-color-lighter); }
.sec-row:last-child { border-bottom: 0; }
.unit { margin-left: 6px; color: var(--el-text-color-secondary); font-size: 12px; }
@media (max-width: 599px) {
  .sec-row { flex-direction: column; align-items: flex-start; }
}
</style>
