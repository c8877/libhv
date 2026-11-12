<template>
  <div>
    <div class="panel">
      <div class="panel-head">
        <h2>上传文件</h2>
        <span class="spacer" />
        <span class="muted">单文件上限 {{ limitMb }} MiB</span>
      </div>
      <div class="upload-row">
        <input ref="fileInput" type="file" class="hidden" @change="onPick" />
        <el-button @click="fileInput.click()">选择文件</el-button>
        <span class="muted file-name">{{ file ? file.name + ' · ' + bytes(file.size) : '未选择' }}</span>
        <el-button type="primary" :disabled="!file || uploading" :loading="uploading" @click="upload">上传</el-button>
      </div>
      <el-progress v-if="uploading" :percentage="progress" :stroke-width="8" class="up-progress" />
      <div v-if="upStatus" class="muted up-status">{{ upStatus }}</div>
    </div>

    <div class="panel">
      <div class="panel-head">
        <h2>文件列表</h2>
        <span class="muted">共 {{ list.length }} 个</span>
        <span class="spacer" />
        <el-input v-model="q" placeholder="搜索文件名" clearable class="search">
          <template #prefix><el-icon><Search /></el-icon></template>
        </el-input>
        <el-button :loading="loading" @click="load">刷新</el-button>
      </div>

      <el-table :data="filtered" v-loading="loading" :default-sort="{ prop: 'name', order: 'ascending' }"
                @sort-change="onSort">
        <el-table-column prop="name" label="名称" min-width="140" sortable="custom">
          <template #default="{ row }"><b>{{ row.name }}</b></template>
        </el-table-column>
        <el-table-column prop="size" label="大小" width="90" align="right" sortable="custom">
          <template #default="{ row }">{{ bytes(row.size) }}</template>
        </el-table-column>
        <el-table-column v-if="!isPhone" label="哈希" min-width="180">
          <template #default="{ row }">
            <code :title="row.sha256 || ''">{{ (row.sha256 || '').slice(0, 8) }}</code>
            <el-button v-if="row.sha256" link type="primary" @click="copy(row.sha256)">复制</el-button>
          </template>
        </el-table-column>
        <el-table-column v-if="!isPhone" prop="mtime_ms" label="修改时间" min-width="170" sortable="custom">
          <template #default="{ row }">{{ time(row.mtime_ms) }}</template>
        </el-table-column>
        <el-table-column :min-width="isPhone ? 92 : 220" label="操作">
          <template #default="{ row }">
            <el-dropdown v-if="isPhone" trigger="click" @command="(c) => onAction(c, row)">
              <el-button size="small">更多<el-icon class="el-icon--right"><ArrowDown /></el-icon></el-button>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="download">下载</el-dropdown-item>
                  <el-dropdown-item v-if="canManage()" command="push" divided>分发</el-dropdown-item>
                  <el-dropdown-item v-if="canManage()" command="del">删除</el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
            <template v-else>
              <el-button size="small" @click="download(row)">下载</el-button>
              <el-button v-if="canManage()" size="small" @click="push(row)">分发</el-button>
              <el-button v-if="canManage()" size="small" type="danger" plain @click="remove(row)">删除</el-button>
            </template>
          </template>
        </el-table-column>
        <template #empty><el-empty description="暂无文件" :image-size="70" /></template>
      </el-table>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '~/shared/api.js'
import { auth, canManage } from '~/shared/store.js'
import { bytes, time } from '~/shared/utils.js'
import { useBreakpoint } from '~/shared/useBreakpoint.js'

const router = useRouter()
const { isPhone } = useBreakpoint()
const list = ref([])
const q = ref('')
const loading = ref(false)
const fileInput = ref(null)
const file = ref(null)
const uploading = ref(false)
const progress = ref(0)
const upStatus = ref('')
const sort = ref({ prop: 'name', order: 'ascending' })

const limitMb = computed(() => parseInt(auth.settings['file.max_upload_mb'] || '32', 10) || 32)
const filtered = computed(() => {
  const k = q.value.trim().toLowerCase()
  return k ? list.value.filter((f) => f.name.toLowerCase().includes(k)) : list.value
})

function sortParam() {
  const map = { ascending: 'asc', descending: 'desc' }
  const prop = sort.value.prop || 'name'
  // 表格列用 mtime_ms，接口参数是 mtime
  const key = prop === 'mtime_ms' ? 'mtime' : prop
  return 'sort=' + key + '&order=' + (map[sort.value.order] || 'asc')
}

async function load() {
  loading.value = true
  try {
    list.value = await Api.get('/api/v1/files?' + sortParam())
  } catch (e) {
    ElMessage.error('加载失败：' + e.message)
  } finally {
    loading.value = false
  }
}

function onSort({ prop, order }) {
  if (!prop || !order) return
  sort.value = { prop, order }
  load()
}

function onPick(e) {
  file.value = e.target.files[0] || null
  upStatus.value = ''
}

function send(url, blob, onprog) {
  return new Promise((res, rej) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', url)
    xhr.setRequestHeader('X-Requested-With', 'fetch')
    xhr.upload.onprogress = (e) => { if (e.lengthComputable && onprog) onprog(e.loaded) }
    xhr.onload = () => (xhr.status === 200 ? res(xhr.responseText)
      : rej(new Error('HTTP ' + xhr.status + ' ' + xhr.responseText)))
    xhr.onerror = () => rej(new Error('网络错误'))
    xhr.send(blob)
  })
}

async function upload() {
  const fi = file.value
  if (!fi) return
  if (fi.size > limitMb.value * 1024 * 1024) {
    ElMessage.error('文件 ' + bytes(fi.size) + ' 超过 ' + limitMb.value + ' MiB 上限')
    return
  }
  uploading.value = true
  progress.value = 0
  upStatus.value = '上传中…'
  const CHUNK = 8 * 1024 * 1024
  const url = '/api/v1/files/' + encodeURIComponent(fi.name)
  try {
    if (fi.size <= CHUNK) {
      await send(url, fi, (loaded) => {
        progress.value = Math.floor((loaded * 100) / fi.size)
        upStatus.value = '上传中… ' + bytes(loaded) + ' / ' + bytes(fi.size)
      })
    } else {
      const uid = Math.random().toString(36).slice(2) + Date.now().toString(36)
      let off = 0
      let tries = 0
      while (off < fi.size) {
        const chunk = fi.slice(off, off + CHUNK)
        try {
          await send(url + '?append=1&upload=' + uid + '&offset=' + off, chunk, (loaded) => {
            const cur = off + loaded
            progress.value = Math.floor((cur * 100) / fi.size)
            upStatus.value = '上传中… ' + bytes(cur) + ' / ' + bytes(fi.size)
          })
          off += chunk.size
          tries = 0
        } catch (e) {
          if (++tries > 2) throw e
          const st = await Api.get('/api/v1/uploads/' + encodeURIComponent(fi.name) + '?upload=' + uid)
          off = st.received || off
          upStatus.value = '重试中… 已传 ' + bytes(off) + ' / ' + bytes(fi.size)
        }
      }
      await send(url + '?finalize=1&upload=' + uid, new Blob([]))
    }
    progress.value = 100
    upStatus.value = '已上传 ' + fi.name + '（' + bytes(fi.size) + '）'
    ElMessage.success('已上传 ' + fi.name)
    file.value = null
    if (fileInput.value) fileInput.value.value = ''
    await load()
  } catch (e) {
    upStatus.value = ''
    ElMessage.error('上传失败：' + e.message)
  } finally {
    uploading.value = false
  }
}

async function download(row) {
  try {
    const r = await fetch('/api/v1/files/' + encodeURIComponent(row.name), { cache: 'no-store' })
    if (!r.ok) throw new Error('HTTP ' + r.status)
    const blob = await r.blob()
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = row.name
    a.click()
    URL.revokeObjectURL(a.href)
  } catch (e) {
    ElMessage.error('下载失败：' + e.message)
  }
}

function push(row) {
  router.push('/distribute?file=' + encodeURIComponent(row.name))
}

async function remove(row) {
  try {
    await ElMessageBox.confirm('删除 ' + row.name + '？', '删除文件',
      { type: 'warning', confirmButtonText: '删除', cancelButtonText: '取消' })
  } catch (e) { return }
  try {
    await Api.del('/api/v1/files/' + encodeURIComponent(row.name))
    ElMessage.success('已删除 ' + row.name)
    await load()
  } catch (e) {
    ElMessage.error('删除失败：' + e.message)
  }
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('哈希已复制')
  } catch (e) {
    ElMessage.warning('复制失败，请手动选择')
  }
}

function onAction(cmd, row) {
  if (cmd === 'download') download(row)
  else if (cmd === 'push') push(row)
  else if (cmd === 'del') remove(row)
}

onMounted(load)
</script>

<style scoped>
.hidden { display: none; }
.upload-row { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.file-name { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.up-progress { margin-top: 12px; max-width: 420px; }
.up-status { margin-top: 6px; }
.search { width: 220px; }
@media (max-width: 599px) {
  .search { width: 100%; }
  .panel-head { flex-wrap: wrap; }
}
</style>
