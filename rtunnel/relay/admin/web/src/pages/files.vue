<template>
  <div class="files-page">
    <section class="panel">
      <div class="panel-head">
        <h2>文件仓库</h2>
        <span class="spacer" />
        <el-tag v-if="loaded" :type="writable ? 'success' : 'info'" size="small">{{ writable ? '读写' : '只读' }}</el-tag>
      </div>
      <div class="mono repo-line">
        relay 本机仓库 · 当前目录 {{ count }} 项 · {{ bytes(total) }}
        <span v-if="loaded && !writable"> · 当前角色只能浏览和下载</span>
        <span v-if="!loaded"> · 正在读取…</span>
      </div>
      <div ref="host" class="repo-host"></div>
    </section>

    <el-dialog v-model="deploy.open" title="部署到设备" width="540px" :close-on-click-modal="false"
               @closed="resetDeploy">
      <el-form label-width="88px" class="deploy-form" @submit.prevent>
        <el-form-item label="目标设备">
          <div class="deploy-dev">
            <el-select v-model="deploy.devid" :disabled="deploy.busy || !agents.length" class="dev-select">
              <el-option v-for="a in agents" :key="a.devid"
                         :label="a.devid + (a.online ? '' : '（离线）')"
                         :value="a.devid" :disabled="!a.online" />
            </el-select>
            <el-tag v-if="deploy.cap === 'rw'" type="success" size="small">可写</el-tag>
            <el-tag v-else-if="deploy.cap === 'loading'" type="info" size="small">读取能力…</el-tag>
            <el-tag v-else type="warning" size="small">{{ capLabel(deploy.cap) }}</el-tag>
          </div>
        </el-form-item>
        <el-form-item label="目标路径">
          <el-input v-model="deploy.path" :disabled="deploy.busy" spellcheck="false" autocomplete="off" />
        </el-form-item>
        <el-form-item label="选项">
          <el-checkbox v-model="deploy.overwrite" :disabled="deploy.busy">覆盖同名文件</el-checkbox>
          <span class="deploy-hint">目标已存在时默认不覆盖</span>
        </el-form-item>
        <el-form-item v-if="deploy.showProgress" label="进度">
          <el-progress :percentage="deploy.pct" :stroke-width="8" class="deploy-progress" />
          <div class="deploy-hint">{{ deploy.msg }}</div>
        </el-form-item>
        <el-form-item v-else-if="deploy.msg" label=" ">
          <span :class="deploy.err ? 'deploy-err' : 'deploy-warn'">{{ deploy.msg }}</span>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="cancelDeploy">{{ deploy.busy ? '取消部署' : '取消' }}</el-button>
        <el-button type="primary" :disabled="deploy.cap !== 'rw'" :loading="deploy.busy" @click="runDeploy">
          {{ deploy.conflict ? '覆盖并部署' : '部署' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { Api } from '@/shared/api.js'
import { auth, canManage, canOperate } from '@/shared/store.js'
import { bytes } from '@/shared/utils.js'
import { createRepoBackend } from '@/shared/repo-backend.js'
import editorUrl from '../../../../../shared/web/cm-editor.bundle.js?url'
import '../../../../../shared/web/table-ui.js'
import '../../../../../shared/web/fs-ui.js'
import '../../../../../shared/web/file-view.js'

const RtunnelFs = window.RtunnelFs
const router = useRouter()

const host = ref(null)
const loaded = ref(false)
const count = ref(0)
const total = ref(0)
let fm = null
let agents = []

const writable = computed(() => canOperate())
const maxUploadMb = computed(() => parseInt(auth.settings['file.max_upload_mb'] || '32', 10) || 32)
const maxEditMb = computed(() => parseInt(auth.settings['file.max_edit_mb'] || '4', 10) || 4)

// --------------------------------------------------------------- 部署

const LAST_DEV = 'rtunnel.files.deployDevid'
const dirKey = (id) => 'rtunnel.files.deployDir.' + id

const deploy = reactive({
  open: false, devid: '', path: '', overwrite: false, cap: 'none',
  busy: false, pct: 0, msg: '', err: false, showProgress: false,
  conflict: false, cancelling: false, repoFile: '', name: '', total: 0,
})
let deployWs = null

function readStore(key, def) {
  try { return localStorage.getItem(key) || def } catch (e) { return def }
}

function writeStore(key, val) {
  try { localStorage.setItem(key, val) } catch (e) { /* ignore */ }
}

function pickDevid() {
  const saved = readStore(LAST_DEV, '')
  const hit = agents.find((a) => a.devid === saved && a.online)
  if (hit) return hit.devid
  const first = agents.find((a) => a.online)
  return first ? first.devid : ''
}

function joinDir(dir, name) {
  const d = (dir || '/').replace(/\/+$/, '')
  return (d === '' ? '' : d) + '/' + name
}

function capLabel(cap) {
  return {
    loading: '读取能力…',
    none: '没有在线设备',
    offline: '设备离线',
    off: '未启用文件服务',
    ro: '只读（agent 未开 --file-rw）',
    unknown: '无法读取设备能力',
    rw: '可写',
  }[cap] || ''
}

async function readCap(devid) {
  const a = agents.find((x) => x.devid === devid)
  if (!devid) return 'none'
  if (!a || !a.online) return 'offline'
  try {
    const r = await Api.get('/api/v1/agents/' + encodeURIComponent(devid) + '/sysinfo?mode=full')
    const c = (r && r.sysinfo && r.sysinfo.caps) || {}
    return c.fs ? (c.fs_rw ? 'rw' : 'ro') : 'off'
  } catch (e) {
    return 'unknown'
  }
}

async function refreshCap() {
  const devid = deploy.devid
  deploy.cap = devid ? 'loading' : 'none'
  const got = await readCap(devid)
  if (deploy.devid !== devid || !deploy.open) return
  deploy.cap = got
}

function openDeploy(name, row, repoPath) {
  if (!canManage()) return
  deploy.repoFile = repoPath || name
  deploy.name = name
  deploy.total = Number(row && row.size) || 0
  const prevDevid = deploy.devid
  deploy.devid = pickDevid()
  deploy.path = joinDir(readStore(dirKey(deploy.devid), '/'), name)
  deploy.overwrite = false
  deploy.busy = false
  deploy.pct = 0
  deploy.msg = ''
  deploy.err = false
  deploy.showProgress = false
  deploy.conflict = false
  deploy.cancelling = false
  deploy.open = true
  // devid 变化时由 watch 刷新；未变化时这里补一次，避免沿用上次的能力状态
  if (deploy.devid === prevDevid) refreshCap()
}

watch(() => deploy.devid, (devid) => {
  if (!deploy.open) return
  deploy.path = joinDir(readStore(dirKey(devid), '/'), deploy.name)
  deploy.overwrite = false
  deploy.msg = ''
  deploy.conflict = false
  refreshCap()
})

function setMsg(text, kind) {
  deploy.msg = text || ''
  deploy.err = kind === 'err'
  deploy.showProgress = false
}

function closeWs() {
  if (!deployWs) return
  try { deployWs.close() } catch (e) { /* ignore */ }
  deployWs = null
}

function resetDeploy() {
  closeWs()
  deploy.busy = false
  deploy.cancelling = false
  deploy.showProgress = false
  deploy.pct = 0
  deploy.msg = ''
}

function cancelDeploy() {
  if (deploy.busy && deployWs) {
    if (deploy.cancelling) return
    try { deployWs.send(JSON.stringify({ op: 'push_cancel', id: 2 })) } catch (e) { /* ignore */ }
    deploy.cancelling = true
    return
  }
  deploy.open = false
}

function runDeploy() {
  const target = deploy.path.trim()
  if (!/^\//.test(target) || target.indexOf('..') >= 0 || target.length > 1024) {
    setMsg('请输入合法的绝对路径', 'err')
    return
  }
  if (deploy.cap !== 'rw') {
    setMsg('目标设备当前不可写', 'err')
    return
  }
  deploy.busy = true
  deploy.cancelling = false
  deploy.conflict = false
  deploy.showProgress = true
  deploy.pct = 0
  deploy.msg = '准备部署…'
  deploy.err = false

  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://'
  const ws = new WebSocket(proto + location.host + '/ws/fs/' + encodeURIComponent(deploy.devid))
  deployWs = ws
  const devid = deploy.devid
  ws.onopen = () => {
    ws.send(JSON.stringify({
      op: 'push_open', id: 1, repo: deploy.repoFile, path: target, overwrite: !!deploy.overwrite,
    }))
  }
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string' || deployWs !== ws) return
    let j
    try { j = JSON.parse(ev.data) } catch (e) { return }
    if (j.op === 'push_progress') {
      const sent = Number(j.sent) || 0
      const all = Number(j.total) || deploy.total
      deploy.pct = all ? Math.min(100, Math.floor((sent * 100) / all)) : 0
      deploy.msg = deploy.pct + '% · ' + bytes(sent) + ' / ' + bytes(all)
    } else if (j.op === 'push_done') {
      deploy.busy = false
      deploy.cancelling = false
      closeWs()
      if (j.ok) {
        writeStore(LAST_DEV, devid)
        const d = target.slice(0, target.lastIndexOf('/')) || '/'
        writeStore(dirKey(devid), d)
        deploy.open = false
        fm.toast('已部署到 ' + devid + ':' + target)
        fm.load('/')
      } else if (j.conflict) {
        deploy.showProgress = false
        deploy.pct = 0
        deploy.conflict = true
        deploy.msg = '目标已存在：' + target + '，勾选「覆盖同名文件」后重试'
        deploy.err = false
      } else {
        setMsg(j.message || '部署失败', 'err')
      }
    } else if (j.op === 'error') {
      deploy.busy = false
      deploy.cancelling = false
      setMsg(j.message || '部署失败', 'err')
      closeWs()
    }
  }
  ws.onerror = () => {
    if (deployWs !== ws) return
    deploy.busy = false
    deploy.cancelling = false
    setMsg('连接失败', 'err')
    closeWs()
  }
  ws.onclose = () => {
    if (deployWs !== ws) return
    deploy.busy = false
    deploy.cancelling = false
    setMsg('连接已断开', 'err')
    deployWs = null
  }
}

// --------------------------------------------------------------- 挂载

function menuExtras(type) {
  if (type !== 'file' || !canManage()) return []
  return [
    { op: 'deploy', label: '部署到设备…' },
    { op: 'distribute', label: '分发到多台设备…' },
  ]
}

function onMenuAction(op, name, type, path) {
  const abs = path || ('/' + name)
  const repoPath = abs.replace(/^\/+/, '')
  if (op === 'deploy') {
    const row = fm ? fm.entryOf(abs) : null
    openDeploy(name, row, repoPath)
    return true
  }
  if (op === 'distribute') {
    router.push('/distribute?file=' + encodeURIComponent(repoPath))
    return true
  }
  return false
}

async function loadAgents() {
  try {
    const arr = await Api.get('/api/v1/agents')
    agents = Array.isArray(arr) ? arr : []
  } catch (e) {
    agents = []
  }
}

function mount() {
  if (!RtunnelFs || !host.value) return
  fm = RtunnelFs.createManager({
    inline: true,
    container: host.value,
    title: '文件仓库',
    height: 'calc(100vh - 230px)',
    searchPlaceholder: '搜索文件名',
    editorUrl,
    backend: createRepoBackend({
      writable: writable.value,
      maxUploadMb: maxUploadMb.value,
      maxEditMb: maxEditMb.value,
      canDelete: canManage(),
      canEdit: canManage(),
    }),
    menuExtras,
    onMenuAction,
    onList: (r) => {
      loaded.value = true
      count.value = (r.entries || []).length
      total.value = (r.entries || []).reduce((n, e) => n + (e.size || 0), 0)
    },
    onSpace: () => { loaded.value = true },
  })
  // 挂载后直接加载仓库根目录
  nextTick(() => fm && fm.open())
}

onMounted(() => {
  loadAgents().then(mount)
})

onBeforeUnmount(() => {
  if (fm) {
    fm.destroy()
    fm = null
  }
})
</script>

<style scoped>
.deploy-dev { display: flex; align-items: center; gap: 10px; width: 100%; }
.dev-select { width: 220px; }
.deploy-hint { margin-left: 8px; font-size: 12px; color: var(--muted); }
.deploy-warn { font-size: 12px; color: var(--el-color-warning); }
.deploy-err { font-size: 12px; color: var(--el-color-danger); }
.deploy-progress { width: 100%; }

.repo-line {
  color: var(--muted);
  font-size: 12px;
  margin-bottom: 10px;
  overflow-wrap: anywhere;
}
</style>
