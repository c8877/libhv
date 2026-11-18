// 文件仓库后端：把 relay-admin 的 HTTP 文件接口适配成 fs-ui 的传输层。
// 与设备文件管理器共用同一份前端组件，只是数据来自 relay 本机仓库（扁平目录）。
import { Api } from './api.js'

const enc = encodeURIComponent

function fileUrl(name) {
  return '/api/v1/files/' + enc(name)
}

// 路径一律使用带前导 / 的仓库路径；relOf 转成接口需要的相对路径
function relOf(path) {
  return String(path || '/').replace(/^\/+/, '').replace(/\/+$/, '')
}

function parentOf(path) {
  const p = String(path || '/')
  const i = p.lastIndexOf('/')
  return i <= 0 ? '/' : p.slice(0, i)
}

function baseOf(path) {
  const p = String(path || '')
  const i = p.lastIndexOf('/')
  return i < 0 ? p : p.slice(i + 1)
}

function dirQuery(dir) {
  return '?dir=' + enc(relOf(dir))
}

function entryOf(f) {
  return {
    name: f.name,
    type: f.type === 'dir' ? 'dir' : 'file',
    size: Number(f.size) || 0,
    mtime: Math.floor((Number(f.mtime_ms) || 0) / 1000),
  }
}

function stamp() {
  const d = new Date()
  const p = (n) => String(n).padStart(2, '0')
  return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' + p(d.getHours()) + p(d.getMinutes())
}

function tarHeader(name, size, mtime) {
  const buf = new Uint8Array(512)
  const enc8 = new TextEncoder()
  const put = (str, off, len) => {
    const b = enc8.encode(str)
    buf.set(b.subarray(0, len), off)
  }
  put(name, 0, 100)
  put('0000644\0', 100, 8)
  put('0000000\0', 108, 8)
  put('0000000\0', 116, 8)
  put(size.toString(8).padStart(11, '0') + '\0', 124, 12)
  put(mtime.toString(8).padStart(11, '0') + '\0', 136, 12)
  buf.fill(0x20, 148, 156)
  buf[156] = 0x30
  put('ustar\0', 257, 6)
  put('00', 263, 2)
  let sum = 0
  for (let i = 0; i < 512; i++) sum += buf[i]
  put(sum.toString(8).padStart(6, '0') + '\0 ', 148, 8)
  return buf
}

// 打包在浏览器里完成：逐个取文件拼 tar，再视情况 gzip；目录结构保留在 tar 内
async function buildArchive(rows, onProgress) {
  const parts = []
  let done = 0
  const total = rows.reduce((n, r) => n + r.size, 0)
  for (const row of rows) {
    const r = await fetch(fileUrl(row.name) + dirQuery(parentOf(row.path)), { cache: 'no-store' })
    if (!r.ok) throw new Error(row.path + ' HTTP ' + r.status)
    const blob = await r.blob()
    const tarName = relOf(row.path) || row.name
    parts.push(tarHeader(tarName, blob.size, Math.floor((row.mtime || Date.now()) / 1000)))
    parts.push(blob)
    const pad = (512 - (blob.size % 512)) % 512
    if (pad) parts.push(new Uint8Array(pad))
    done += blob.size
    if (onProgress) onProgress(done, total)
  }
  parts.push(new Uint8Array(1024))
  let out = new Blob(parts, { type: 'application/x-tar' })
  let ext = '.tar'
  if ('CompressionStream' in window) {
    try {
      out = await new Response(out.stream().pipeThrough(new CompressionStream('gzip'))).blob()
      ext = '.tar.gz'
    } catch (e) {
      ext = '.tar'
    }
  }
  const base = rows.length === 1 ? baseOf(rows[0].path) : ('repo-' + rows.length)
  return { blob: out, name: base.replace(/\.[^.]+$/, '') + '-' + stamp() + ext }
}

function sendChunk(url, blob, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', url)
    xhr.setRequestHeader('X-Requested-With', 'fetch')
    xhr.upload.onprogress = (e) => { if (e.lengthComputable && onProgress) onProgress(e.loaded) }
    xhr.onload = () => (xhr.status === 200 ? resolve(xhr.responseText)
      : reject(new Error(parseErr(xhr.responseText, xhr.status))))
    xhr.onerror = () => reject(new Error('网络错误'))
    xhr.send(blob)
  })
}

function parseErr(text, status) {
  try {
    const j = JSON.parse(text)
    if (j && j.error) return j.error
  } catch (e) { /* ignore */ }
  return 'HTTP ' + status
}

export function createRepoBackend(opts = {}) {
  const maxEditMb = Number(opts.maxEditMb) || 4

  return {
    connect() {
      return Promise.resolve({
        label: '文件仓库',
        root: '/',
        rw: !!opts.writable,
        caps: {
          flat: false, tree: true, hidden: false, dirSize: false,
          mkdir: !!opts.writable, touch: false,
          rename: !!opts.writable, move: !!opts.writable,
          pack: true, rowEdit: opts.canEdit === undefined ? !!opts.writable : !!opts.canEdit,
          upload: !!opts.writable,
          del: opts.canDelete === undefined ? !!opts.writable : !!opts.canDelete,
          edit: opts.canEdit === undefined ? !!opts.writable : !!opts.canEdit,
          archive: true,
        },
      })
    },

    list(path) {
      const p = path || '/'
      return Api.get('/api/v1/files' + dirQuery(p)).then((arr) => ({
        path: p,
        root: '/',
        rw: !!opts.writable,
        label: '文件仓库',
        entries: (arr || []).map(entryOf),
      }))
    },

    // 递归搜索子目录，返回带完整相对路径的条目
    search(dir, q) {
      return Api.get('/api/v1/files?recursive=1&dir=' + enc(relOf(dir)) + '&q=' + enc(q)).then((arr) =>
        (arr || []).map((f) => {
          const rel = String(f.path || f.name).replace(/^\/+/, '')
          return {
            name: rel,
            path: '/' + rel,
            type: f.type === 'dir' ? 'dir' : 'file',
            size: Number(f.size) || 0,
            mtime: Math.floor((Number(f.mtime_ms) || 0) / 1000),
          }
        }))
    },

    stat(path) {
      const name = baseOf(path)
      return this.list(parentOf(path)).then((r) => {
        const hit = r.entries.find((x) => x.name === name)
        if (!hit) throw new Error('文件不存在：' + path)
        return { entry: hit }
      })
    },

    size() {
      return Promise.resolve({ bytes: 0, truncated: false })
    },

    mkdir(path) {
      const name = baseOf(path)
      if (!name) return Promise.reject(new Error('目录名不能为空'))
      return Api.post(fileUrl(name) + '?action=mkdir&dir=' + enc(relOf(parentOf(path))), {})
    },

    touch() {
      return Promise.reject(new Error('仓库不支持新建空文件'))
    },

    rename(path, newName) {
      const dir = parentOf(path)
      const to = (relOf(dir) ? relOf(dir) + '/' : '') + newName
      return Api.post(fileUrl(baseOf(path)) + '?action=move&dir=' + enc(relOf(dir)) + '&to=' + enc(to), {})
    },

    move(path, destDir) {
      const dir = parentOf(path)
      const name = baseOf(path)
      const to = (relOf(destDir) ? relOf(destDir) + '/' : '') + name
      return Api.post(fileUrl(name) + '?action=move&dir=' + enc(relOf(dir)) + '&to=' + enc(to), {})
    },

    remove(path, recursive) {
      return Api.del(fileUrl(baseOf(path)) + dirQuery(parentOf(path)) + (recursive ? '&recursive=1' : ''))
    },

    writeText(path, text) {
      const name = baseOf(path)
      const size = new TextEncoder().encode(text).length
      if (size > maxEditMb * 1024 * 1024) {
        return Promise.reject(new Error('文件超过 ' + maxEditMb + ' MiB，请下载后编辑'))
      }
      return fetch(fileUrl(name) + dirQuery(parentOf(path)), {
        method: 'PUT',
        headers: { 'Content-Type': 'text/plain', 'X-Requested-With': 'fetch' },
        body: text,
      }).then(async (r) => {
        if (!r.ok) throw new Error(parseErr(await r.text(), r.status))
        return r.json().catch(() => null)
      })
    },

    download(path, onProgress, opts) {
      const name = baseOf(path)
      const headers = {}
      if (opts && (opts.offset || opts.limit)) {
        const off = opts.offset || 0
        const end = opts.limit ? off + opts.limit - 1 : ''
        headers.Range = 'bytes=' + off + '-' + end
      }
      return fetch(fileUrl(name) + dirQuery(parentOf(path)), { cache: 'no-store', headers }).then(async (r) => {
        if (r.status !== 200 && r.status !== 206) throw new Error(parseErr(await r.text(), r.status))
        const total = Number(r.headers.get('Content-Length')) || 0
        if (!onProgress || !r.body || !r.body.getReader) {
          const blob = await r.blob()
          if (onProgress) onProgress(blob.size, blob.size)
          return { meta: { name, size: blob.size }, blob }
        }
        const reader = r.body.getReader()
        const chunks = []
        let got = 0
        for (;;) {
          const { done, value } = await reader.read()
          if (done) break
          chunks.push(value)
          got += value.length
          onProgress(got, total || got)
        }
        const blob = new Blob(chunks)
        return { meta: { name, size: blob.size }, blob }
      })
    },

    readRange(path, offset, len) {
      return fetch(fileUrl(baseOf(path)) + dirQuery(parentOf(path)), {
        cache: 'no-store',
        headers: { Range: 'bytes=' + offset + '-' + (offset + len - 1) },
      }).then(async (r) => {
        if (r.status !== 200 && r.status !== 206) throw new Error(parseErr(await r.text(), r.status))
        return new Uint8Array(await r.arrayBuffer())
      })
    },

    upload(file, dir, onProgress, cancelled) {
      const url = fileUrl(file.name) + dirQuery(dir)
      const maxMb = Number(opts.maxUploadMb) || 32
      if (file.size > maxMb * 1024 * 1024) return Promise.reject(new Error('超过 ' + maxMb + ' MiB 上限'))
      const CHUNK = 8 * 1024 * 1024
      let sent = 0
      const tick = (n) => { sent = n; if (onProgress) onProgress(sent, file.size) }
      if (cancelled && cancelled()) return Promise.reject(new Error('已取消'))
      if (file.size <= CHUNK) {
        return sendChunk(url, file, tick).then(() => tick(file.size))
      }
      const uid = Math.random().toString(36).slice(2) + Date.now().toString(36)
      let off = 0
      let tries = 0
      const step = () => {
        if (cancelled && cancelled()) return Promise.reject(new Error('已取消'))
        if (off >= file.size) {
          return sendChunk(url + '&finalize=1&upload=' + uid, new Blob([])).then(() => tick(file.size))
        }
        const chunk = file.slice(off, off + CHUNK)
        return sendChunk(url + '&append=1&upload=' + uid + '&offset=' + off, chunk, (n) => tick(off + n))
          .then(() => { off += chunk.size; tries = 0; return step() })
          .catch(async (e) => {
            if (++tries > 2) throw e
            const st = await Api.get('/api/v1/uploads/' + enc(file.name) + '?upload=' + uid + '&dir=' + enc(relOf(dir)))
            off = Number(st && st.received) || off
            return step()
          })
      }
      return step()
    },

    archive(paths, onProgress) {
      return Promise.all(paths.map((p) => this.stat(p).then((r) => {
        if (r.entry.type === 'dir') throw new Error('目录暂不支持打包')
        return { path: p, name: baseOf(p), size: r.entry.size, mtime: r.entry.mtime }
      }))).then((rows) => buildArchive(rows, onProgress))
    },

    close() {},
  }
}
