<template>
  <div class="panel">
    <div class="panel-head">
      <h2>部署</h2>
      <span class="muted">用 SSH / SFTP 把 agent、relay、client 装到内网主机</span>
      <span class="spacer" />
      <el-button @click="openScan">扫描网段</el-button>
      <el-button @click="openHost()">登记主机</el-button>
      <el-button :loading="refreshing" @click="reload">刷新</el-button>
    </div>

    <el-tabs v-model="tab" @tab-change="onTab">
      <!-- ------------------------------------------------ 部署 -->
      <el-tab-pane label="部署" name="new">
        <el-form ref="formRef" :model="form" :rules="rules" label-width="104px" class="dep-form">
          <div class="sec"><span class="sec-no">1</span>目标主机
            <span class="muted">选一台登记过的主机，选中会自动探测环境</span>
          </div>

          <el-form-item label="主机" prop="host_id">
            <el-select v-model="form.host_id" placeholder="选择主机，或先扫描网段 / 登记新主机"
                       class="w340" filterable @change="onHostChange">
              <el-option v-for="h in hosts" :key="h.id" :value="h.id" :label="hostLabel(h)">
                <span class="dot" :class="hostDot(h)">{{ hostStateText(h) }}</span>
                <span class="mono">{{ h.name || h.host }}</span>
                <span class="muted opt-note">{{ h.username }}@{{ h.host }}:{{ h.port }}</span>
              </el-option>
            </el-select>
            <el-button :disabled="!form.host_id" :loading="probing" @click="probe">测试连接</el-button>
          </el-form-item>

          <el-form-item v-if="form.host_id" label="目标环境">
            <div class="probe">
              <template v-if="probing">
                <span class="muted">正在连接目标主机…</span>
              </template>
              <template v-else-if="probeResult && probeResult.ok">
                <el-tag type="success" size="small">可连接</el-tag>
                <span class="mono">{{ probeResult.arch || '架构未知' }}</span>
                <el-tag size="small" effect="plain" :type="probeResult.systemd ? 'success' : 'info'">
                  {{ probeResult.systemd ? 'systemd' : '无 systemd' }}
                </el-tag>
                <el-tag size="small" effect="plain" :type="probeResult.uid === 0 ? 'success' : 'warning'">
                  {{ probeResult.uid === 0 ? 'root 登录' : '非 root，需要 sudo' }}
                </el-tag>
                <span class="muted">/tmp 剩余 {{ Math.round((probeResult.free_kb || 0) / 1024) }} MiB</span>
                <el-tag v-if="probeResult.installed && probeResult.installed !== 'none'" size="small" effect="plain">
                  已装 {{ probeResult.agent_status || probeResult.relay_status || probeResult.installed }}
                </el-tag>
              </template>
              <template v-else-if="probeResult">
                <el-tag type="danger" size="small">连接失败</el-tag>
                <span class="err-text">{{ probeResult.error }}</span>
                <el-button link type="primary" @click="probe">重试</el-button>
              </template>
              <template v-else>
                <span class="muted">还没有探测，点「测试连接」先确认能连上、架构对不对</span>
              </template>
            </div>
          </el-form-item>

          <div class="sec"><span class="sec-no">2</span>安装内容
            <span class="muted">安装包来自文件仓库，和 OTA 用的是同一份</span>
          </div>

          <el-form-item label="组件">
            <el-radio-group v-model="form.component" @change="onComponent">
              <el-radio-button value="agent">agent（设备接入）</el-radio-button>
              <el-radio-button value="relay">relay（中继节点）</el-radio-button>
              <el-radio-button value="client">client（隧道客户端）</el-radio-button>
            </el-radio-group>
          </el-form-item>

          <el-form-item label="安装包" prop="package">
            <div class="row">
              <el-select v-model="form.package" placeholder="选择安装包" class="w340" filterable>
                <el-option v-for="p in componentPkgs" :key="p.path" :value="p.path"
                           :label="pkgLabel(p)">
                  <span class="mono">{{ pkgLabel(p) }}</span>
                  <el-tag v-if="archMismatch(p)" size="small" type="warning" effect="plain" class="ml4">
                    架构不符
                  </el-tag>
                </el-option>
              </el-select>
              <el-upload :show-file-list="false" :http-request="uploadPkg" accept=".tar.gz">
                <el-button :loading="uploading">上传安装包</el-button>
              </el-upload>
              <span v-if="componentPkgs.length === 0" class="muted">
                文件仓库 packages/ 下还没有 {{ form.component }} 安装包，先上传
              </span>
            </div>
            <div v-if="uploadPct > 0 && uploadPct < 100" class="muted">上传中 {{ uploadPct }}%</div>
            <el-alert v-if="selectedPkg && probeResult && probeResult.ok && selectedPkg.arch !== probeResult.arch"
                      class="ml0" type="warning" :closable="false" show-icon
                      :title="`安装包是 ${selectedPkg.arch}，目标机是 ${probeResult.arch}：预检会拒绝，换对应架构的包`" />
          </el-form-item>

          <div class="sec"><span class="sec-no">3</span>接入参数
            <span class="muted">按组件不同，只需要填这几项</span>
          </div>

          <template v-if="form.component === 'agent'">
            <el-form-item label="设备 ID" prop="devid">
              <el-input v-model="form.devid" placeholder="唯一标识，如 board01" class="w340" maxlength="64" />
              <span class="muted unit">接入后设备列表里的名字，不能和已有设备重复</span>
            </el-form-item>
            <el-form-item label="relay 地址" prop="server">
              <el-input v-model="form.server" placeholder="host:port，如 10.0.0.10:18443" class="w340" />
              <span class="muted unit">数据面地址，不是管理台端口</span>
            </el-form-item>
            <el-form-item label="允许端口">
              <el-input v-model="form.allow" placeholder="22,8080 或留空" class="w340" />
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
              <el-input v-model="form.admin_token" placeholder="留空自动生成" class="w340" />
              <span class="muted unit">登录管理台的初始密码，生成后会写进作业日志</span>
            </el-form-item>
          </template>

          <template v-else>
            <el-form-item label="relay 地址">
              <el-input v-model="form.client_server" placeholder="host:port" class="w340" />
              <span class="muted unit">写入 /etc/rtunnel/client.env 的 SERVER</span>
            </el-form-item>
          </template>

          <el-collapse v-model="advanced" class="adv">
            <el-collapse-item title="高级：安装目录与服务接管" name="adv">
              <el-form-item label="安装目录">
                <el-input v-model="form.prefix" :placeholder="defaultPrefix" class="w340" />
                <span class="muted unit">留空用默认 {{ defaultPrefix }}</span>
              </el-form-item>
              <el-form-item label="服务接管">
                <el-checkbox v-model="form.no_units">只安装文件，不接管 systemd</el-checkbox>
                <div class="tip muted">容器、临时目录或已有自己的启动方式时用；勾选后不会 enable/start</div>
              </el-form-item>
            </el-collapse-item>
          </el-collapse>

          <div class="dep-actions">
            <div class="summary">
              <div class="muted">{{ summary }}</div>
              <div class="muted">安装脚本会做版本自检与 sha256 校验，失败会保留日志</div>
            </div>
            <el-button type="primary" size="large" :loading="starting" :disabled="!canDeploy"
                       @click="startDeploy">
              开始部署
            </el-button>
          </div>
        </el-form>

        <div class="hint muted">
          页面上的「上传安装包」会按包名落到文件仓库 packages/&lt;组件&gt;/&lt;版本&gt;/&lt;架构&gt;/。
          部署在后台执行：预检 → 传包到 /tmp → 解压 → 写配置 → 包内 install.sh → 验证 → 清理，
          日志在「记录」里实时看。SSH/SFTP 由 relay 内置的 libssh2 完成，目标机不需要 ssh 客户端。
        </div>
      </el-tab-pane>

      <!-- ------------------------------------------------ 主机 -->
      <el-tab-pane :label="`主机 ${hosts.length}`" name="hosts">
        <div class="bar">
          <span class="muted">凭据只存在 relay 本机（0600），接口不会回显密码或私钥</span>
          <span class="spacer" />
          <el-button @click="openScan">扫描网段</el-button>
          <el-button type="primary" @click="openHost()">登记主机</el-button>
        </div>
        <el-table :data="hosts" v-loading="loadingHosts">
          <el-table-column label="名称" width="280">
            <template #default="{ row }">
              <div>{{ row.name || row.host }}</div>
              <div class="muted mono">{{ row.username }}@{{ row.host }}:{{ row.port }}</div>
            </template>
          </el-table-column>
          <el-table-column label="认证" width="140">
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
          <el-table-column prop="note" label="备注" min-width="200">
            <template #default="{ row }">{{ row.note || '—' }}</template>
          </el-table-column>
          <el-table-column label="操作" width="160">
            <template #default="{ row }">
              <el-button link type="primary" @click="deployOn(row)">部署</el-button>
              <el-dropdown trigger="click" @command="(c) => hostCmd(c, row)">
                <el-button link type="primary" class="more">更多</el-button>
                <template #dropdown>
                  <el-dropdown-menu>
                    <el-dropdown-item command="probe">测试连接</el-dropdown-item>
                    <el-dropdown-item command="edit">编辑</el-dropdown-item>
                    <el-dropdown-item command="del" divided>删除</el-dropdown-item>
                  </el-dropdown-menu>
                </template>
              </el-dropdown>
            </template>
          </el-table-column>
          <template #empty>
            <el-empty description="还没有登记主机：可以扫描网段自动发现，或手动登记一台" :image-size="70">
              <el-button @click="openScan">扫描网段</el-button>
              <el-button type="primary" @click="openHost()">登记主机</el-button>
            </el-empty>
          </template>
        </el-table>
      </el-tab-pane>

      <!-- ------------------------------------------------ 记录 -->
      <el-tab-pane :label="jobsTabLabel" name="jobs">
        <div class="bar">
          <el-radio-group v-model="jobsFilter" size="small">
            <el-radio-button value="all">全部 {{ jobs.length }}</el-radio-button>
            <el-radio-button value="active">进行中 {{ activeJobs }}</el-radio-button>
            <el-radio-button value="success">成功 {{ successJobs }}</el-radio-button>
            <el-radio-button value="failed">失败 {{ failedJobs }}</el-radio-button>
          </el-radio-group>
          <span class="spacer" />
          <span class="muted">点一行看实时日志；进行中的作业会自动刷新</span>
        </div>
        <el-table :data="visibleJobs" v-loading="loadingJobs" @row-click="openJob">
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
            <el-empty :description="jobsFilter === 'all' ? '还没有部署记录' : '这个状态下没有作业'"
                      :image-size="70" />
          </template>
        </el-table>
      </el-tab-pane>
    </el-tabs>

    <!-- 网段扫描 -->
    <el-dialog v-model="scanDlg" title="扫描网段里的 SSH 主机" width="660px">
      <el-form inline class="scan-form" @submit.prevent>
        <el-form-item label="网段">
          <el-input v-model="scanForm.cidr" placeholder="192.168.1.0/24" class="w240"
                    @keyup.enter="doScan" />
        </el-form-item>
        <el-form-item label="端口">
          <el-input-number v-model="scanForm.port" :min="1" :max="65535"
                           controls-position="right" class="w120" />
        </el-form-item>
        <el-button type="primary" :loading="scanning" @click="doScan">开始扫描</el-button>
      </el-form>
      <div class="muted scan-hint">
        在 relay 主机上跑 nmap（-Pn -T4，只探这一个端口）；没装 nmap 的精简镜像是内置 TCP 连接 +
        读 SSH 横幅。只允许私有网段，最多 1024 个地址。
      </div>
      <el-alert v-if="scanError" :title="scanError" type="error" :closable="false" show-icon />
      <template v-if="scanResult">
        <div class="muted scan-sum">
          {{ scanResult.engine === 'nmap' ? 'nmap' : '内置 TCP' }} ·
          扫了 {{ scanResult.scanned }} 个地址 · 用时
          {{ (scanResult.duration_ms / 1000).toFixed(1) }} 秒 ·
          找到 {{ scanResult.items.length }} 台
        </div>
        <el-table :data="scanResult.items" size="small" max-height="320">
          <el-table-column label="地址" width="150">
            <template #default="{ row }"><span class="mono">{{ row.host }}</span></template>
          </el-table-column>
          <el-table-column label="端口" width="90">
            <template #default="{ row }"><span class="mono">{{ row.port }}</span></template>
          </el-table-column>
          <el-table-column label="服务" width="100">
            <template #default="{ row }">
              <el-tag size="small" :type="row.service === 'ssh' ? 'success' : 'info'" effect="plain">
                {{ row.service }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column label="状态" width="110">
            <template #default="{ row }">
              <span v-if="row.known" class="muted">已登记</span>
              <span v-else>待登记</span>
            </template>
          </el-table-column>
          <el-table-column label="操作">
            <template #default="{ row }">
              <el-button v-if="row.known" link type="primary" @click="scanDeploy(row)">去部署</el-button>
              <el-button v-else link type="primary" @click="scanAdd(row)">登记</el-button>
            </template>
          </el-table-column>
          <template #empty>
            <el-empty description="这个网段没有开着该端口的主机" :image-size="60" />
          </template>
        </el-table>
      </template>
    </el-dialog>

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
          <el-button size="small" @click="copyLog">复制日志</el-button>
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
        <pre ref="logEl" class="log mono">{{ logJob.log || '（等待输出）' }}</pre>
      </template>
    </el-drawer>
  </div>
</template>

<script setup>
// 运维 → 部署：SSH/SFTP 直连目标主机，把文件仓库里的安装包装上去并执行包内 install.sh。
// 凭据只在 relay 本机，前端只提交、不回读。
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { Api } from '@/shared/api.js'
import { time } from '@/shared/utils.js'

const tab = ref('new')
const hosts = ref([])
const jobs = ref([])
const pkgs = ref([])
const loadingHosts = ref(false)
const loadingJobs = ref(false)
const refreshing = ref(false)
const probing = ref(false)
const probeResult = ref(null)
const starting = ref(false)
const uploading = ref(false)
const uploadPct = ref(0)
const advanced = ref([])
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

// 架构匹配的包排前面，其次是最新上传的（后端已按 mtime 倒序）
const componentPkgs = computed(() => {
  const list = pkgs.value.filter((p) => p.component === form.component)
  const arch = probeResult.value && probeResult.value.ok ? probeResult.value.arch : ''
  if (!arch) return list
  return [...list].sort((a, b) => (a.arch === arch ? 0 : 1) - (b.arch === arch ? 0 : 1))
})

const selectedPkg = computed(() => pkgs.value.find((p) => p.path === form.package) || null)

const canDeploy = computed(() => !!(form.host_id && form.package))

function pkgLabel(p) {
  let size = ''
  if (p.size >= 1048576) size = (p.size / 1048576).toFixed(1) + ' MiB'
  else if (p.size >= 1024) size = Math.round(p.size / 1024) + ' KiB'
  return [p.version, p.arch, p.variant, size].filter(Boolean).join(' · ')
}

function archMismatch(p) {
  return !!(probeResult.value && probeResult.value.ok && probeResult.value.arch &&
    p.arch !== probeResult.value.arch)
}

const activeJobs = computed(() =>
  jobs.value.filter((j) => j.status === 'running' || j.status === 'queued').length)

const jobsTabLabel = computed(() =>
  activeJobs.value > 0 ? `记录 ${jobs.value.length} · ${activeJobs.value} 运行中`
    : `记录 ${jobs.value.length}`)

const successJobs = computed(() => jobs.value.filter((j) => j.status === 'success').length)
const failedJobs = computed(() => jobs.value.filter((j) => j.status === 'failed').length)

const jobsFilter = ref('all')
const visibleJobs = computed(() => {
  if (jobsFilter.value === 'all') return jobs.value
  if (jobsFilter.value === 'active')
    return jobs.value.filter((j) => j.status === 'running' || j.status === 'queued')
  if (jobsFilter.value === 'success') return jobs.value.filter((j) => j.status === 'success')
  return jobs.value.filter((j) => j.status === 'failed')
})

// 提交前一眼看清“装什么、装到哪、怎么接管”
const summary = computed(() => {
  const h = hosts.value.find((x) => x.id === form.host_id)
  if (!h) return '先选目标主机'
  const what = selectedPkg.value
    ? `${selectedPkg.value.component} ${selectedPkg.value.version}（${selectedPkg.value.arch}）`
    : '未选安装包'
  const where = form.prefix.trim() || defaultPrefix.value
  const mode = form.no_units ? '只装文件'
    : (probeResult.value && probeResult.value.ok && !probeResult.value.systemd
      ? '目标机没有 systemd，仅装文件' : '安装并接管服务')
  return `${what} → ${h.username}@${h.host}:${h.port} → ${where} · ${mode}`
})

function defaultDevid(h) {
  const fromName = (h.name || '').replace(/[^A-Za-z0-9._-]/g, '').replace(/^[-._]+|[-._]+$/g, '')
  if (fromName.length >= 3) return fromName.slice(0, 40)
  return (h.host || 'device').replace(/[^A-Za-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '').slice(0, 40) || 'device'
}

function hostLabel(h) {
  return `${h.name || h.host}（${h.username}@${h.host}）`
}

function hostDot(h) {
  return { ok: 'ok', unreachable: 'bad' }[h.last_status] || 'unknown'
}

function hostStateText(h) {
  return { ok: '可连接', unreachable: '连不上' }[h.last_status] || '未测试'
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

async function reload() {
  refreshing.value = true
  try {
    await Promise.all([loadHosts(), loadPkgs(), loadJobs()])
  } finally {
    refreshing.value = false
  }
}

function onTab(name) {
  if (name === 'hosts') loadHosts()
  if (name === 'jobs') loadJobs()
  if (name === 'new') loadPkgs()
}

function onComponent() {
  // 换组件后包一定不通用，清掉让用户重新选（也避免按旧包的架构误判）
  form.package = ''
}

function onHostChange() {
  probeResult.value = null
  const h = hosts.value.find((x) => x.id === form.host_id)
  if (h && !form.devid) {
    // 默认拿主机名当设备 ID，名字全是中文就退回主机地址，用户可改
    form.devid = defaultDevid(h)
  }
  // 选中就顺带探一下：早发现连不上、架构不符、没有 sudo，不用等到点「开始部署」
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

function probeHost(row) {
  form.host_id = row.id
  tab.value = 'new'
  onHostChange()
}

function deployOn(row) {
  form.host_id = row.id
  tab.value = 'new'
  if (pkgs.value.length === 0) loadPkgs()
  onHostChange()
}

function hostCmd(cmd, row) {
  if (cmd === 'probe') probeHost(row)
  else if (cmd === 'edit') openHost(row)
  else if (cmd === 'del') removeHost(row)
}

// ---------------------------------------------------------------- 网段扫描
const scanDlg = ref(false)
const scanning = ref(false)
const scanError = ref('')
const scanResult = ref(null)
const scanForm = reactive({ cidr: '', port: 22 })

// 用已选主机（或第一台）的 /24 当默认网段，省得手敲
function guessCidr() {
  const h = hosts.value.find((x) => x.id === form.host_id) || hosts.value[0]
  const m = h && /^(\d+\.\d+\.\d+)\.\d+$/.exec(h.host || '')
  return m ? `${m[1]}.0/24` : ''
}

function openScan() {
  scanError.value = ''
  scanResult.value = null
  if (!scanForm.cidr) scanForm.cidr = guessCidr()
  scanDlg.value = true
}

async function doScan() {
  const cidr = scanForm.cidr.trim()
  if (!cidr) {
    ElMessage.warning('先填要扫的网段')
    return
  }
  scanning.value = true
  scanError.value = ''
  scanResult.value = null
  try {
    scanResult.value = await Api.post('/api/v1/deploy/scan', { cidr, port: scanForm.port })
    if (scanResult.value.items.length === 0) ElMessage.info('这个网段没有开着该端口的主机')
  } catch (e) {
    scanError.value = e.message
  } finally {
    scanning.value = false
  }
}

// 扫到的主机直接带进登记表单，省掉再敲一遍地址
function scanAdd(row) {
  scanDlg.value = false
  openHost()
  hostForm.name = row.host
  hostForm.host = row.host
  hostForm.port = row.port
}

function scanDeploy(row) {
  scanDlg.value = false
  form.host_id = row.host_id
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
    let newId = 0
    if (hostForm.id) {
      await Api.put(`/api/v1/deploy/hosts/${hostForm.id}`, body)
      ElMessage.success('已保存')
    } else {
      const r = await Api.post('/api/v1/deploy/hosts', body)
      newId = r.id
      ElMessage.success('已登记')
    }
    hostDlg.value = false
    await loadHosts()
    if (newId) {
      // 新登记的主机直接选中并探测，少一步手点
      form.host_id = newId
      onHostChange()
    }
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
    if (form.host_id === row.id) {
      form.host_id = null
      probeResult.value = null
    }
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
    const hit = pkgs.value.find((p) => p.name === file.name)
    form.package = hit ? hit.path : file.name
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
  if (selectedPkg.value && archMismatch(selectedPkg.value)) {
    try {
      await ElMessageBox.confirm(
        `安装包是 ${selectedPkg.value.arch}，目标机是 ${probeResult.value.arch}，预检会拒绝这次部署。仍然提交？`,
        '架构不匹配', { type: 'warning' })
    } catch (e) {
      return
    }
  }
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
    jobsFilter.value = 'all'
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
const logEl = ref(null)

async function openJob(row) {
  logJob.value = row
  logDlg.value = true
  await refreshJob(row.id)
  scrollLogToEnd()
  startPoll()
}

async function refreshJob(id) {
  try {
    const j = await Api.get('/api/v1/deploy/jobs/' + id)
    logJob.value = j
  } catch (e) { /* 保留上一次内容 */ }
}

// 日志追加时，如果本来就在底部就跟着滚；用户往上翻过就不打扰
async function scrollLogToEnd(force) {
  await nextTick()
  const el = logEl.value
  if (!el) return
  const near = el.scrollHeight - el.scrollTop - el.clientHeight < 80
  if (force || near) el.scrollTop = el.scrollHeight
}

watch(() => (logJob.value ? logJob.value.log : ''), () => scrollLogToEnd(false))

async function copyLog() {
  const txt = (logJob.value && logJob.value.log) || ''
  try {
    await navigator.clipboard.writeText(txt)
    ElMessage.success('日志已复制')
  } catch (e) {
    ElMessage.error('复制失败：' + e.message)
  }
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
    // 停在记录页时也刷新，别人发起的作业也能看到进度
    if (active || logDlg.value || tab.value === 'jobs') {
      await loadJobs()
      if (logDlg.value && logJob.value) await refreshJob(logJob.value.id)
    }
    if (!active && !logDlg.value && tab.value !== 'jobs') stopPoll()
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
.dep-form { max-width: 940px; }
.row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.w240 { width: 240px; }
.w300 { width: 300px; }
.w340 { width: 340px; }
.w120 { width: 120px; }
.unit { margin-left: 10px; }
.port { width: 120px; margin-left: 10px; }
.ml0 { margin-left: 0; }
.ml4 { margin-left: 4px; }
.opt-note { margin-left: 12px; }
.tip { flex-basis: 100%; margin-top: 2px; }
.sec {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 22px 0 12px;
  font-weight: 600;
}
.sec:first-child { margin-top: 0; }
.sec-no {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 20px;
  height: 20px;
  border-radius: 50%;
  background: var(--el-color-primary-light-9);
  color: var(--el-color-primary);
  font-size: 12px;
}
.sec .muted { font-weight: 400; }
.adv { margin-top: 18px; }
.dep-actions {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-top: 20px;
  padding-top: 16px;
  border-top: 1px solid var(--el-border-color-lighter);
}
.summary { flex: 1; line-height: 1.7; word-break: break-all; }
.probe { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.dot {
  display: inline-block;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  margin-right: 8px;
  background: var(--el-text-color-placeholder);
}
.dot.ok { background: var(--el-color-success); }
.dot.bad { background: var(--el-color-danger); }
.err-text { color: var(--el-color-danger); font-size: 12.5px; word-break: break-all; }
.bar { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
.hint { margin-top: 22px; line-height: 1.8; }
.ver { margin-left: 8px; }
.more { margin-left: 8px; }
.job-head { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
.job-meta { margin-bottom: 12px; }
.job-err { margin-bottom: 10px; }
.scan-form { margin-bottom: 4px; }
.scan-hint { line-height: 1.7; margin-bottom: 12px; }
.scan-sum { margin: 4px 0 10px; }
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
