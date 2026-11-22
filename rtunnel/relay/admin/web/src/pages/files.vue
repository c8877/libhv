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
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
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

function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text !== undefined) e.textContent = text
  return e
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

function openDeploy(name, row, repoPath) {
  if (!canManage()) return
  const repoFile = repoPath || name
  const absPath = '/' + String(repoFile).replace(/^\/+/, '')
  const size = Number(row && row.size) || 0
  let devid = pickDevid()
  let cap = devid ? 'loading' : 'none'
  let busy = false
  let ws = null

  const panel = fm.inlinePanel(absPath, ({ box, close }) => {
    const r1 = el('div', 'row')
    r1.appendChild(el('span', 'lb', '目标设备'))
    const sel = el('select')
    for (const a of agents) {
      const o = el('option', '', a.devid + (a.online ? '' : '（离线）'))
      o.value = a.devid
      o.disabled = !a.online
      if (a.devid === devid) o.selected = true
      sel.appendChild(o)
    }
    if (!agents.length) {
      const o = el('option', '', '没有在线设备')
      o.value = ''
      sel.appendChild(o)
    }
    sel.disabled = busy
    const capTx = el('span', cap === 'rw' ? 'hint' : 'warn', capLabel(cap))
    r1.appendChild(sel)
    r1.appendChild(capTx)

    const r2 = el('div', 'row')
    r2.appendChild(el('span', 'lb', '目标路径'))
    const inp = el('input')
    inp.type = 'text'
    inp.spellcheck = false
    inp.autocomplete = 'off'
    inp.value = joinDir(readStore(dirKey(devid), '/'), name)
    inp.disabled = busy
    r2.appendChild(inp)

    const r3 = el('div', 'row')
    r3.appendChild(el('span', 'lb', '选项'))
    const lab = el('label', 'chk')
    const ow = el('input')
    ow.type = 'checkbox'
    ow.disabled = busy
    lab.appendChild(ow)
    lab.appendChild(document.createTextNode(' 覆盖同名文件'))
    r3.appendChild(lab)
    r3.appendChild(el('span', 'hint', '目标已存在时默认不覆盖'))

    const r4 = el('div', 'row')
    const msg = el('span', 'hint', '')
    const prog = el('div', 'prog')
    const bar = el('i')
    prog.appendChild(bar)
    prog.hidden = true
    const ok = el('button', 'primary', '部署')
    ok.disabled = cap !== 'rw'
    const no = el('button', '', '取消')
    r4.appendChild(msg)
    r4.appendChild(prog)
    r4.appendChild(el('span', 'sp'))
    r4.appendChild(ok)
    r4.appendChild(no)

    box.appendChild(r1)
    box.appendChild(r2)
    box.appendChild(r3)
    box.appendChild(r4)

    function setMsg(text, kind) {
      msg.textContent = text || ''
      msg.className = kind === 'err' ? 'err' : kind === 'warn' ? 'warn' : 'hint'
    }

    async function refreshCap() {
      cap = devid ? 'loading' : 'none'
      capTx.textContent = capLabel(cap)
      capTx.className = 'hint'
      ok.disabled = true
      const got = await readCap(devid)
      if (!box.isConnected) return
      cap = got
      capTx.textContent = capLabel(cap)
      capTx.className = cap === 'rw' ? 'hint' : 'warn'
      ok.disabled = busy || cap !== 'rw'
    }

    sel.addEventListener('change', () => {
      devid = sel.value
      inp.value = joinDir(readStore(dirKey(devid), '/'), name)
      ow.checked = false
      setMsg('')
      refreshCap()
    })

    no.addEventListener('click', () => {
      if (busy && ws) {
        try { ws.send(JSON.stringify({ op: 'push_cancel', id: 2 })) } catch (e) { /* ignore */ }
        setMsg('正在取消…')
        return
      }
      close()
    })

    ok.addEventListener('click', () => {
      const target = inp.value.trim()
      if (!/^\//.test(target) || target.indexOf('..') >= 0 || target.length > 1024) {
        setMsg('请输入合法的绝对路径', 'err')
        return
      }
      if (cap !== 'rw') {
        setMsg('目标设备当前不可写', 'err')
        return
      }
      busy = true
      ok.disabled = true
      no.textContent = '取消'
      sel.disabled = true
      inp.disabled = true
      ow.disabled = true
      prog.hidden = false
      bar.style.width = '0%'
      setMsg('准备部署…')

      const proto = location.protocol === 'https:' ? 'wss://' : 'ws://'
      ws = new WebSocket(proto + location.host + '/ws/fs/' + encodeURIComponent(devid))
      ws.onopen = () => {
        ws.send(JSON.stringify({ op: 'push_open', id: 1, repo: repoFile, path: target, overwrite: !!ow.checked }))
      }
      ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') return
        let j
        try { j = JSON.parse(ev.data) } catch (e) { return }
        if (j.op === 'push_progress') {
          const sent = Number(j.sent) || 0
          const all = Number(j.total) || size
          const pct = all ? Math.floor((sent * 100) / all) : 0
          bar.style.width = pct + '%'
          setMsg(pct + '% · ' + bytes(sent) + ' / ' + bytes(all))
        } else if (j.op === 'push_done') {
          busy = false
          try { ws.close() } catch (e) { /* ignore */ }
          ws = null
          if (j.ok) {
            writeStore(LAST_DEV, devid)
            const d = target.slice(0, target.lastIndexOf('/')) || '/'
            writeStore(dirKey(devid), d)
            close()
            fm.toast('已部署到 ' + devid + ':' + target)
            fm.load('/')
          } else if (j.conflict) {
            bar.style.width = '0%'
            prog.hidden = true
            ok.disabled = false
            ok.textContent = '覆盖并部署'
            sel.disabled = false
            inp.disabled = false
            ow.disabled = false
            no.textContent = '取消'
            setMsg('目标已存在：' + target + '，勾选「覆盖同名文件」后重试', 'warn')
          } else {
            bar.style.width = '0%'
            prog.hidden = true
            sel.disabled = false
            inp.disabled = false
            ow.disabled = false
            ok.disabled = false
            no.textContent = '取消'
            setMsg(j.message || '部署失败', 'err')
          }
        } else if (j.op === 'error') {
          busy = false
          bar.style.width = '0%'
          prog.hidden = true
          sel.disabled = false
          inp.disabled = false
          ow.disabled = false
          ok.disabled = false
          no.textContent = '取消'
          setMsg(j.message || '部署失败', 'err')
          try { ws.close() } catch (e) { /* ignore */ }
          ws = null
        }
      }
      ws.onerror = () => {
        if (!busy) return
        busy = false
        prog.hidden = true
        ok.disabled = false
        sel.disabled = false
        inp.disabled = false
        ow.disabled = false
        no.textContent = '取消'
        setMsg('连接失败', 'err')
      }
      ws.onclose = () => {
        if (!busy) return
        busy = false
        prog.hidden = true
        ok.disabled = false
        sel.disabled = false
        inp.disabled = false
        ow.disabled = false
        no.textContent = '取消'
        setMsg('连接已断开', 'err')
      }
    })

    refreshCap()
  })

  return panel
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
.repo-line {
  color: var(--muted);
  font-size: 12px;
  margin-bottom: 10px;
  overflow-wrap: anywhere;
}
</style>
