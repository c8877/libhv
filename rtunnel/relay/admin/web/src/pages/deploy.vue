<template>
  <div class="panel">
    <div class="panel-head">
      <h2>部署</h2>
      <span class="muted">通过 SSH / SCP 把 agent、relay、client 直接装到内网主机</span>
      <span class="spacer" />
      <el-button @click="reload">刷新</el-button>
    </div>

    <el-tabs v-model="tab" @tab-change="onTab">
      <!-- ------------------------------------------------ 新建部署 -->
      <el-tab-pane label="新建部署" name="new">
        <el-form ref="formRef" :model="form" :rules="rules" label-width="108px" class="dep-form">
          <el-form-item label="目标主机" prop="host_id">
            <div class="row">
              <el-select v-model="form.host_id" placeholder="选择一台已登记的主机" class="w300"
                         @change="onHostChange">
                <el-option v-for="h in hosts" :key="h.id" :value="h.id" :label="hostLabel(h)">
                  <span class="mono">{{ h.username }}@{{ h.host }}:{{ h.port }}</span>
                  <span class="muted opt-note">{{ h.name }}</span>
                </el-option>
              </el-select>
              <el-button @click="openHost()">登记新主机</el-button>
              <el-button :disabled="!form.host_id" :loading="probing" @click="probe">测试连接</el-button>
            </div>
          </el-form-item>

          <el-form-item v-if="probeResult" label="目标环境">
            <div class="probe">
              <el-tag v-if="probeResult.ok" type="success" size="small">可连接</el-tag>
              <el-tag v-else type="danger" size="small">连接失败</el-tag>
              <template v-if="probeResult.ok">
                <span class="mono">{{ probeResult.arch || '?' }}</span>
                <span class="muted">{{ probeResult.systemd ? 'systemd' : '无 systemd' }}</span>
                <span class="muted">{{ probeResult.uid === 0 ? 'root 登录' : '非 root（需要 sudo）' }}</span>
                <span class="muted">/tmp 剩余 {{ Math.round((probeResult.free_kb || 0) / 1024) }} MiB</span>
                <span v-if="probeResult.installed && probeResult.installed !== 'none'" class="muted">
                  已装：{{ probeResult.agent_status || probeResult.relay_status || probeResult.installed }}
                </span>
              </template>
              <span v-else class="err-text">{{ probeResult.error }}</span>
            </div>
          </el-form-item>

          <el-form-item label="安装组件">
            <el-radio-group v-model="form.component" @change="onComponent">
              <el-radio-button value="agent">agent（设备接入）</el-radio-button>
              <el-radio-button value="relay">relay（中继节点）</el-radio-button>
              <el-radio-button value="client">client（隧道客户端）</el-radio-button>
            </el-radio-group>
          </el-form-item>

          <el-form-item label="安装包" prop="package">
            <div class="row">
              <el-select v-model="form.package" placeholder="选择安装包" class="w300" filterable>
                <el-option v-for="p in componentPkgs" :key="p.name" :value="p.name"
                           :label="`${p.name}（${p.version} ${p.arch}）`" />
              </el-select>
              <el-upload :show-file-list="false" :http-request="uploadPkg" accept=".tar.gz">
                <el-button :loading="uploading">上传安装包</el-button>
              </el-upload>
              <span v-if="componentPkgs.length === 0" class="muted">
                还没有 {{ form.component }} 安装包，先上传
              </span>
            </div>
            <div v-if="uploadPct > 0 && uploadPct < 100" class="muted">上传中 {{ uploadPct }}%</div>
          </el-form-item>

          <!-- agent 需要的身份与 relay 地址 -->
          <template v-if="form.component === 'agent'">
            <el-form-item label="设备 ID" prop="devid">
              <el-input v-model="form.devid" placeholder="唯一标识，如 board01" class="w300" maxlength="64" />
              <span class="muted unit">接入后设备列表里的名字，不能和已有设备重复</span>
            </el-form-item>
            <el-form-item label="relay 地址" prop="server">
              <el-input v-model="form.server" placeholder="host:port，如 10.0.0.10:18443" class="w300" />
              <span class="muted unit">数据面地址，不是管理台端口</span>
            </el-form-item>
            <el-form-item label="允许端口">
              <el-input v-model="form.allow" placeholder="22,8080 或留空" class="w300" />
              <span class="muted unit">设备侧可被转发的端口，留空表示不开放</span>
            </el-form-item>
            <el-form-item label="变体">
              <el-radio-group v-model="form.variant">
                <el-radio-button value="web">web（带设备本地页）</el-radio-button>
                <el-radio-button value="lite">lite（体积小）</el-radio-button>
              </el-radio-group>
            </el-form-item>
          </template>

          <template v-else-if="form.component === 'relay'">
            <el-form-item label="管理端口">
              <el-input-number v-model="form.admin_port" :min="1" :max="65535" controls-position="right" />
              <span class="muted unit">管理台 HTTPS 端口，默认 9443（自签证书）</span>
            </el-form-item>
            <el-form-item label="初始令牌">
              <el-input v-model="form.admin_token" placeholder="留空自动生成" class="w300" />
              <span class="muted unit">登录管理台的初始密码，生成后会写进作业日志</span>
            </el-form-item>
          </template>

          <template v-else>
            <el-form-item label="relay 地址">
              <el-input v-model="form.client_server" placeholder="host:port" class="w300" />
              <span class="muted unit">写入 /etc/rtunnel/client.env 的 SERVER</span>
            </el-form-item>
          </template>

          <el-form-item label="安装目录">
            <el-input v-model="form.prefix" :placeholder="defaultPrefix" class="w300" />
            <span class="muted unit">留空用默认 {{ defaultPrefix }}</span>
          </el-form-item>

          <el-form-item label="服务接管">
            <el-checkbox v-model="form.no_units">只安装文件，不接管 systemd</el-checkbox>
            <span class="muted unit">容器、临时目录或已有自己的启动方式时用；勾选后不会 enable/start 服务</span>
          </el-form-item>

          <el-form-item>
            <el-button type="primary" :loading="starting" @click="startDeploy">开始部署</el-button>
            <span class="muted unit">安装脚本会做版本自检与 sha256 校验，失败会保留日志</span>
          </el-form-item>
        </el-form>

        <div class="hint muted">
          部署过程：预检（连通性/架构/sudo）→ 上传安装包到 /tmp → 解压 → 写配置 → 执行包内
          install.sh → 验证版本与服务状态。整个过程在后台跑，可以在「记录」里看实时日志。
          SSH/SFTP 由 relay 内置的 libssh2 完成，目标机不需要装 ssh 客户端、sshpass 或 agent。
        </div>
      </el-tab-pane>

      <!-- ------------------------------------------------ 主机 -->
      <el-tab-pane :label="`主机 ${hosts.length}`" name="hosts">
        <div class="bar">
          <span class="muted">凭据只存在 relay 本机（0600），接口不会回显密码或私钥</span>
          <span class="spacer" />
          <el-button type="primary" @click="openHost()">登记主机</el-button>
        </div>
        <el-table :data="hosts" v-loading="loadingHosts">
          <el-table-column label="名称" min-width="150">
            <template #default="{ row }">
              <div>{{ row.name || row.host }}</div>
              <div class="muted mono">{{ row.username }}@{{ row.host }}:{{ row.port }}</div>
            </template>
          </el-table-column>
          <el-table-column label="认证" width="110">
            <template #default="{ row }">
              <el-tag size="small" effect="plain">{{ row.auth === 'key' ? '私钥' : '密码' }}</el-tag>
              <el-tag v-if="row.sudo" size="small" effect="plain" class="ml4">sudo</el-tag>
              <el-tag v-if="!row.secret_set" size="small" type="warning" class="ml4">缺凭据</el-tag>
            </template>
          </el-table-column>
          <el-table-column label="最近状态" width="150">
            <template #default="{ row }">
              <el-tag v-if="row.last_status === 'ok'" type="success" size="small">可连接</el-tag>
              <el-tag v-else-if="row.last_status === 'unreachable'" type="danger" size="small">连不上</el-tag>
              <span v-else class="muted">未测试</span>
              <div v-if="row.last_ms" class="muted">{{ time(row.last_ms) }}</div>
            </template>
          </el-table-column>
          <el-table-column prop="note" label="备注" min-width="140">
            <template #default="{ row }">{{ row.note || '—' }}</template>
          </el-table-column>
          <el-table-column label="操作" width="220">
            <template #default="{ row }">
              <el-button link type="primary" @click="probeHost(row)">测试</el-button>
              <el-button link type="primary" @click="deployOn(row)">部署</el-button>
              <el-button link type="primary" @click="openHost(row)">编辑</el-button>
              <el-button link type="danger" @click="removeHost(row)">删除</el-button>
            </template>
          </el-table-column>
          <template #empty>
            <el-empty description="还没有登记主机" :image-size="70" />
          </template>
        </el-table>
      </el-tab-pane>

      <!-- ------------------------------------------------ 记录 -->
      <el-tab-pane :label="`记录 ${jobs.length}`" name="jobs">
        <el-table :data="jobs" v-loading="loadingJobs" @row-click="openJob">
          <el-table-column prop="id" label="#" width="70" />
          <el-table-column label="主机" min-width="160">
            <template #default="{ row }">
              <span class="mono">{{ row.username }}@{{ row.host }}</span>
            </template>
          </el-table-column>
          <el-table-column label="组件" width="200">
            <template #default="{ row }">
              <el-tag size="small" effect="plain">{{ row.component }}</el-tag>
              <span class="muted mono ver">{{ row.version }} · {{ row.arch }}</span>
            </template>
          </el-table-column>
          <el-table-column label="状态" width="130">
            <template #default="{ row }">
              <el-tag :type="statusType(row.status)" size="small">{{ statusText(row.status) }}</el-tag>
              <div class="muted">{{ row.step }}</div>
            </template>
          </el-table-column>
          <el-table-column prop="actor" label="发起人" width="120" />
          <el-table-column label="时间" width="170">
            <template #default="{ row }">
              <div>{{ time(row.created_ms) }}</div>
              <div v-if="row.finished_ms" class="muted">用时 {{ dur(row) }}</div>
            </template>
          </el-table-column>
          <el-table-column label="操作" width="120">
            <template #default="{ row }">
              <el-button link type="primary" @click.stop="openJob(row)">日志</el-button>
              <el-button v-if="row.status === 'running' || row.status === 'queued'" link type="danger"
                         @click.stop="cancelJob(row)">取消</el-button>
            </template>
          </el-table-column>
          <template #empty>
            <el-empty description="还没有部署记录" :image-size="70" />
          </template>
        </el-table>
      </el-tab-pane>
    </el-tabs>

    <!-- 主机表单 -->
    <el-dialog v-model="hostDlg" :title="hostForm.id ? '编辑主机' : '登记主机'" width="560px">
      <el-form ref="hostRef" :model="hostForm" :rules="hostRules" label-width="96px">
        <el-form-item label="名称" prop="name">
          <el-input v-model="hostForm.name" placeholder="便于识别的名字，如 车间-01" maxlength="64" />
        </el-form-item>
        <el-form-item label="地址" prop="host">
          <el-input v-model="hostForm.host" placeholder="IP 或域名" class="w240" />
          <el-input-number v-model="hostForm.port" :min="1" :max="65535" controls-position="right"
                           class="port" />
        </el-form-item>
        <el-form-item label="登录用户" prop="username">
          <el-input v-model="hostForm.username" placeholder="root 或带 sudo 的账号" class="w240" />
        </el-form-item>
        <el-form-item label="认证方式">
          <el-radio-group v-model="hostForm.auth">
            <el-radio-button value="password">密码</el-radio-button>
            <el-radio-button value="key">私钥</el-radio-button>
          </el-radio-group>
        </el-form-item>
        <el-form-item :label="hostForm.auth === 'key' ? '私钥' : '密码'"
                      :prop="hostForm.id ? '' : 'secret'">
          <el-input v-model="hostForm.secret" type="password" show-password
                    :placeholder="hostForm.id ? '留空表示不修改' : (hostForm.auth === 'key' ? '粘贴私钥全文' : '登录密码')" />
        </el-form-item>
        <el-form-item label="sudo">
          <el-switch v-model="hostForm.sudo" />
          <span class="muted unit">登录用户不是 root 时，安装需要 sudo</span>
        </el-form-item>
        <el-form-item label="备注">
          <el-input v-model="hostForm.note" maxlength="120" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="hostDlg = false">取消</el-button>
        <el-button type="primary" :loading="savingHost" @click="saveHost">保存</el-button>
      </template>
    </el-dialog>

    <!-- 作业日志 -->
    <el-drawer v-model="logDlg" :title="logJob ? `部署 #${logJob.id}` : '部署'" size="620px">
      <template v-if="logJob">
        <div class="job-head">
          <el-tag :type="statusType(logJob.status)" size="small">{{ statusText(logJob.status) }}</el-tag>
          <span class="mono">{{ logJob.component }} {{ logJob.version }}</span>
          <span class="muted">{{ logJob.username }}@{{ logJob.host }}</span>
          <span class="spacer" />
          <el-button v-if="logJob.status === 'running' || logJob.status === 'queued'"
                     size="small" type="danger" @click="cancelJob(logJob)">取消</el-button>
        </div>
        <el-descriptions :column="2" border size="small" class="job-meta">
          <el-descriptions-item label="安装包">{{ logJob.pkg }}</el-descriptions-item>
          <el-descriptions-item label="架构">{{ logJob.arch || '—' }}</el-descriptions-item>
          <el-descriptions-item label="安装目录">{{ logJob.prefix }}</el-descriptions-item>
          <el-descriptions-item label="发起人">{{ logJob.actor || '—' }}</el-descriptions-item>
          <el-descriptions-item label="开始">{{ logJob.started_ms ? time(logJob.started_ms) : '—' }}</el-descriptions-item>
          <el-descriptions-item label="结束">{{ logJob.finished_ms ? time(logJob.finished_ms) : '—' }}</el-descriptions-item>
        </el-descriptions>
        <div v-if="logJob.error" class="err-text job-err">{{ logJob.error }}</div>
        <pre class="log mono">{{ logJob.log || '（等待输出）' }}</pre>
      </template>
    </el-drawer>
  </div>
</template>

<script setup>
// 运维 → 部署：SSH/SCP 直连目标主机，把安装包装上去并执行包内 install.sh。
// 凭据只在 relay 本机，前端只提交、不回读。
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { time } from '@/shared/utils.js'

const tab = ref('new')
const hosts = ref([])
const jobs = ref([])
const pkgs = ref([])
const loadingHosts = ref(false)
const loadingJobs = ref(false)
const probing = ref(false)
const probeResult = ref(null)
const starting = ref(false)
const uploading = ref(false)
const uploadPct = ref(0)
const formRef = ref(null)

const form = reactive({
  host_id: null,
  component: 'agent',
  package: '',
  variant: 'web',
  devid: '',
  server: '',
  allow: '22',
  admin_port: 9443,
  admin_token: '',
  client_server: '',
  prefix: '',
  no_units: false,
})

const rules = {
  host_id: [{ required: true, message: '先选一台目标主机', trigger: 'change' }],
  package: [{ required: true, message: '选择安装包', trigger: 'change' }],
  devid: [{ required: true, message: '设备 ID 不能为空', trigger: 'blur' },
    { pattern: /^[A-Za-z0-9._-]{1,64}$/, message: '只能用字母数字和 . _ -', trigger: 'blur' }],
  server: [{ required: true, message: '填 relay 的 host:port', trigger: 'blur' }],
}

const defaultPrefix = computed(() => ({
  agent: '/opt/rtunnel/agent',
  relay: '/opt/rtunnel/relay',
  client: '/opt/rtunnel/client',
}[form.component]))

const componentPkgs = computed(() => pkgs.value.filter((p) => p.component === form.component))

function hostLabel(h) {
  return `${h.name || h.host}（${h.username}@${h.host}）`
}

function statusType(s) {
  return ({ queued: 'info', running: 'warning', success: 'success', failed: 'danger', canceled: 'info' }[s] || 'info')
}

function statusText(s) {
  return ({ queued: '排队', running: '执行中', success: '成功', failed: '失败', canceled: '已取消' }[s] || s)
}

function dur(j) {
  if (!j.started_ms || !j.finished_ms) return '—'
  const s = Math.max(0, Math.round((j.finished_ms - j.started_ms) / 1000))
  return s < 60 ? s + ' 秒' : Math.floor(s / 60) + ' 分 ' + (s % 60) + ' 秒'
}

async function loadHosts() {
  loadingHosts.value = true
  try {
    hosts.value = (await Api.get('/api/v1/deploy/hosts')).items || []
  } catch (e) {
    ElMessage.error('读取主机失败：' + e.message)
  } finally {
    loadingHosts.value = false
  }
}

async function loadPkgs() {
  try {
    pkgs.value = (await Api.get('/api/v1/deploy/packages')).items || []
  } catch (e) {
    pkgs.value = []
  }
}

async function loadJobs() {
  loadingJobs.value = true
  try {
    jobs.value = (await Api.get('/api/v1/deploy/jobs?limit=50')).items || []
  } catch (e) {
    ElMessage.error('读取部署记录失败：' + e.message)
  } finally {
    loadingJobs.value = false
  }
}

function reload() {
  loadHosts()
  loadPkgs()
  loadJobs()
}

function onTab(name) {
  if (name === 'hosts') loadHosts()
  if (name === 'jobs') loadJobs()
  if (name === 'new') loadPkgs()
}

function onComponent() {
  form.package = ''
  probeResult.value = null
}

function onHostChange() {
  probeResult.value = null
  const h = hosts.value.find((x) => x.id === form.host_id)
  if (h && !form.devid) {
    // 默认拿主机名当设备 ID，用户可改
    form.devid = (h.name || h.host).replace(/[^A-Za-z0-9._-]/g, '-').slice(0, 40)
  }
  // 选中就顺带探一下：早发现连不上/架构不符，不用等到点「开始部署」
  if (h && h.secret_set) probe()
}

async function probe() {
  if (!form.host_id) return
  probing.value = true
  probeResult.value = null
  try {
    probeResult.value = await Api.post(`/api/v1/deploy/hosts/${form.host_id}/probe`, {})
    if (!probeResult.value.ok) ElMessage.error(probeResult.value.error || '连接失败')
    else {
      if (probeResult.value.arch && form.server === '') {
        form.server = `${location.hostname || '127.0.0.1'}:18443`
      }
      loadHosts()
    }
  } catch (e) {
    probeResult.value = { ok: false, error: e.message }
    ElMessage.error('连接失败：' + e.message)
  } finally {
    probing.value = false
  }
}

async function probeHost(row) {
  form.host_id = row.id
  tab.value = 'new'
  await probe()
}

function deployOn(row) {
  form.host_id = row.id
  tab.value = 'new'
  onHostChange()
}

// ---------------------------------------------------------------- 主机表单
const hostDlg = ref(false)
const savingHost = ref(false)
const hostRef = ref(null)
const hostForm = reactive({
  id: 0, name: '', host: '', port: 22, username: 'root',
  auth: 'password', sudo: false, note: '', secret: '',
})
const hostRules = {
  name: [{ required: true, message: '给这台主机起个名字', trigger: 'blur' }],
  host: [{ required: true, message: '填 IP 或域名', trigger: 'blur' }],
  username: [{ required: true, message: '填登录用户', trigger: 'blur' }],
  secret: [{ required: true, message: '填密码或私钥', trigger: 'blur' }],
}

function openHost(row) {
  Object.assign(hostForm, {
    id: row ? row.id : 0,
    name: row ? row.name : '',
    host: row ? row.host : '',
    port: row ? row.port : 22,
    username: row ? row.username : 'root',
    auth: row ? row.auth : 'password',
    sudo: row ? row.sudo : false,
    note: row ? row.note : '',
    secret: '',
  })
  hostDlg.value = true
}

async function saveHost() {
  const ok = await hostRef.value.validate().catch(() => false)
  if (!ok) return
  savingHost.value = true
  try {
    const body = {
      name: hostForm.name, host: hostForm.host, port: hostForm.port,
      username: hostForm.username, auth: hostForm.auth, sudo: hostForm.sudo,
      note: hostForm.note,
    }
    if (hostForm.secret) body.secret = hostForm.secret
    if (hostForm.id) {
      await Api.put(`/api/v1/deploy/hosts/${hostForm.id}`, body)
      ElMessage.success('已保存')
    } else {
      const r = await Api.post('/api/v1/deploy/hosts', body)
      form.host_id = r.id
      ElMessage.success('已登记')
    }
    hostDlg.value = false
    await loadHosts()
  } catch (e) {
    ElMessage.error('保存失败：' + e.message)
  } finally {
    savingHost.value = false
  }
}

async function removeHost(row) {
  try {
    await ElMessageBox.confirm(`删除主机「${row.name || row.host}」？凭据也会一起删掉。`, '删除主机',
      { type: 'warning' })
  } catch (e) {
    return
  }
  try {
    await Api.del(`/api/v1/deploy/hosts/${row.id}`)
    ElMessage.success('已删除')
    loadHosts()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

// ---------------------------------------------------------------- 上传安装包
async function putChunk(url, blob) {
  const r = await fetch(url, {
    method: 'POST',
    body: blob,
    headers: { 'X-Requested-With': 'fetch' },
  })
  if (!r.ok) {
    let msg = 'HTTP ' + r.status
    try {
      const j = await r.json()
      if (j && j.error) msg = j.error
    } catch (e) { /* 非 JSON */ }
    throw new Error(msg)
  }
}

async function uploadPkg(opt) {
  const file = opt.file
  if (!/\.tar\.gz$/i.test(file.name)) {
    ElMessage.warning('安装包必须是 .tar.gz')
    return
  }
  uploading.value = true
  uploadPct.value = 0
  try {
    const base = '/api/v1/deploy/packages/' + encodeURIComponent(file.name)
    const CHUNK = 8 * 1024 * 1024
    if (file.size <= CHUNK) {
      await putChunk(base, file)
      uploadPct.value = 100
    } else {
      const uid = Math.random().toString(36).slice(2) + Date.now().toString(36)
      let off = 0
      while (off < file.size) {
        const chunk = file.slice(off, off + CHUNK)
        await putChunk(`${base}?append=1&upload=${uid}&offset=${off}`, chunk)
        off += chunk.size
        uploadPct.value = Math.round((off / file.size) * 100)
      }
      await putChunk(`${base}?finalize=1&upload=${uid}`, new Blob([]))
      uploadPct.value = 100
    }
    await loadPkgs()
    form.package = file.name
    ElMessage.success('已上传 ' + file.name)
  } catch (e) {
    ElMessage.error('上传失败：' + e.message)
  } finally {
    uploading.value = false
    setTimeout(() => { uploadPct.value = 0 }, 1200)
  }
}

// ---------------------------------------------------------------- 提交部署
async function startDeploy() {
  const ok = await formRef.value.validate().catch(() => false)
  if (!ok) return
  const options = {}
  if (form.component === 'agent') {
    options.devid = form.devid
    options.server = form.server
    options.allow = form.allow
  } else if (form.component === 'relay') {
    options.admin_port = form.admin_port
    if (form.admin_token) options.admin_token = form.admin_token
  } else if (form.client_server) {
    options.server = form.client_server
  }
  starting.value = true
  try {
    const r = await Api.post('/api/v1/deploy/jobs', {
      host_id: form.host_id,
      component: form.component,
      package: form.package,
      variant: form.variant,
      prefix: form.prefix.trim(),
      no_units: form.no_units,
      options,
    })
    ElMessage.success('已提交部署 #' + r.id)
    form.admin_token = ''
    await loadJobs()
    const job = jobs.value.find((j) => j.id === r.id)
    tab.value = 'jobs'
    if (job) openJob(job)
    startPoll()
  } catch (e) {
    ElMessage.error('提交失败：' + e.message)
  } finally {
    starting.value = false
  }
}

// ---------------------------------------------------------------- 作业与日志
const logDlg = ref(false)
const logJob = ref(null)

function openJob(row) {
  logJob.value = row
  logDlg.value = true
  refreshJob(row.id)
  startPoll()
}

async function refreshJob(id) {
  try {
    const j = await Api.get('/api/v1/deploy/jobs/' + id)
    logJob.value = j
  } catch (e) { /* 保留上一次内容 */ }
}

async function cancelJob(row) {
  try {
    await ElMessageBox.confirm('取消后目标机上的安装会中断，可能留下半装状态。继续？', '取消部署',
      { type: 'warning' })
  } catch (e) {
    return
  }
  try {
    await Api.post(`/api/v1/deploy/jobs/${row.id}/cancel`, {})
    ElMessage.success('已请求取消')
    refreshJob(row.id)
    loadJobs()
  } catch (e) {
    ElMessage.error('取消失败：' + e.message)
  }
}

let timer = null
function startPoll() {
  if (timer) return
  timer = setInterval(async () => {
    const active = jobs.value.some((j) => j.status === 'running' || j.status === 'queued')
    if (active || logDlg.value) {
      await loadJobs()
      if (logDlg.value && logJob.value) await refreshJob(logJob.value.id)
    }
    if (!active && !logDlg.value) stopPoll()
  }, 2500)
}
function stopPoll() {
  if (timer) { clearInterval(timer); timer = null }
}

onMounted(() => {
  reload()
  startPoll()
})
onBeforeUnmount(stopPoll)
</script>

<style scoped>
.dep-form { max-width: 900px; }
.row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.w300 { width: 300px; }
.unit { margin-left: 10px; }
.port { width: 120px; margin-left: 10px; }
.ml4 { margin-left: 4px; }
.opt-note { margin-left: 12px; }
.probe { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.err-text { color: var(--el-color-danger); font-size: 12.5px; word-break: break-all; }
.bar { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
.hint { margin-top: 18px; line-height: 1.8; }
.ver { margin-left: 8px; }
.job-head { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
.job-meta { margin-bottom: 12px; }
.job-err { margin-bottom: 10px; }
.log {
  background: var(--el-fill-color-light);
  border-radius: 8px;
  padding: 12px;
  font-size: 12px;
  line-height: 1.7;
  max-height: calc(100vh - 340px);
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
</style>
