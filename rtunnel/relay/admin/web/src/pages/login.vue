<template>
  <div class="login-wrap">
    <div class="login-card">
      <div class="login-logo">◈ rtunnel</div>
      <h1>{{ auth.setupDone ? '登录' : '初始化超级管理员' }}</h1>
      <p class="sub">{{ auth.setupDone ? '使用管理台账号登录' : '首次部署，创建第一个管理员' }}</p>

      <el-form :model="form" label-position="top" @submit.prevent="submit">
        <el-form-item v-if="!auth.setupDone" label="部署 token">
          <el-input id="af-token" v-model="form.token" type="password" show-password placeholder="启动 relay 时配置的 --admin-token" />
        </el-form-item>
        <el-form-item label="用户名">
          <el-input id="af-user" v-model="form.username" autocomplete="username" placeholder="用户名" />
        </el-form-item>
        <el-form-item label="密码">
          <el-input id="af-pass" v-model="form.password" type="password" show-password autocomplete="current-password"
                    placeholder="密码" @keyup.enter="submit" />
        </el-form-item>

        <el-alert v-if="error" :title="error" type="error" show-icon :closable="false" class="mb12" />

        <el-button id="af-btn" type="primary" class="submit" :loading="loading" @click="submit">
          {{ auth.setupDone ? '登录' : '创建并登录' }}
        </el-button>
      </el-form>
    </div>
  </div>
</template>

<script setup>
import { reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { auth, login, setup } from '@/shared/store.js'

const router = useRouter()
const form = reactive({ token: '', username: '', password: '' })
const loading = ref(false)
const error = ref('')

async function submit() {
  if (loading.value) return
  error.value = ''
  if (!form.username || !form.password) {
    error.value = '请填写用户名和密码'
    return
  }
  if (!auth.setupDone && !form.token) {
    error.value = '请填写部署 token'
    return
  }
  loading.value = true
  try {
    if (auth.setupDone) await login(form.username, form.password)
    else await setup(form.token, form.username, form.password)
    ElMessage.success(auth.setupDone ? '已登录' : '管理员已创建')
    const want = auth.profile && auth.profile.default_page === 'agents' ? '/agents' : '/overview'
    router.replace(want)
  } catch (e) {
    error.value = e.message || '操作失败'
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.login-wrap {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  background:
    radial-gradient(900px 480px at 6% -12%, color-mix(in srgb, var(--el-color-primary) 22%, transparent), transparent 62%),
    radial-gradient(760px 420px at 108% 112%, rgba(124, 58, 237, 0.16), transparent 60%),
    var(--el-bg-color-page);
}
.login-card {
  width: min(400px, 100%);
  padding: 32px 32px 28px;
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 18px;
  box-shadow: 0 32px 64px -34px rgba(15, 23, 42, 0.38), 0 2px 6px rgba(15, 23, 42, 0.04);
}
.login-logo {
  display: inline-flex;
  align-items: center;
  padding: 6px 10px;
  border-radius: 9px;
  background: var(--el-color-primary-light-9);
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.08em;
  color: var(--el-color-primary);
}
h1 {
  margin: 16px 0 6px;
  font-size: 24px;
  letter-spacing: -0.01em;
}
.sub {
  margin: 0 0 22px;
  color: var(--el-text-color-secondary);
  font-size: 13px;
}
.mb12 {
  margin-bottom: 12px;
}
.submit {
  width: 100%;
  height: 40px;
}

@media (max-width: 599px) {
  .login-card { padding: 26px 22px 22px; border-radius: 16px; }
  h1 { font-size: 21px; }
}
</style>
