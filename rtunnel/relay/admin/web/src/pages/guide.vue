<template>
  <div class="guide">
    <div class="panel">
      <div class="panel-head">
        <h2>使用指南</h2>
        <span class="spacer" />
        <el-button text @click="$router.push('/docs')">接口文档</el-button>
      </div>
      <div class="muted lead">
        这份指南按“装起来 → 接设备 → 分权限 → 日常运维 → 接 AI”的顺序走。
        每一步都能跳到对应页面，命令可直接复制。
      </div>

      <div class="toc">
        <a v-for="s in SECTIONS" :key="s.id" class="toc-item" @click="go(s.id)">{{ s.title }}</a>
      </div>
    </div>

    <div v-for="s in SECTIONS" :id="s.id" :key="s.id" class="panel sec">
      <div class="panel-head"><h2>{{ s.title }}</h2></div>
      <div class="sec-body">
        <div v-for="(b, i) in s.blocks" :key="i" class="blk">
          <div v-if="b.h" class="sub">{{ b.h }}</div>
          <div v-if="b.p" class="para" v-html="b.p" />
          <ul v-if="b.ul" class="ul">
            <li v-for="(li, j) in b.ul" :key="j" v-html="li" />
          </ul>
          <div v-if="b.code" class="code-wrap">
            <pre class="code">{{ b.code }}</pre>
            <el-button size="small" text class="copy" @click="copy(b.code)">复制</el-button>
          </div>
          <el-alert v-if="b.tip" :type="b.tipType || 'info'" :closable="false" class="tip"
                    :title="b.tip" />
          <div v-if="b.links" class="links">
            <el-button v-for="l in b.links" :key="l.to" size="small" @click="$router.push(l.to)">
              {{ l.label }}
            </el-button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ElMessage } from 'element-plus'

const SECTIONS = [
  {
    id: 'arch',
    title: '1. 这套系统是什么',
    blocks: [
      {
        p: 'rtunnel 是一套<b>反向隧道 + 远程运维</b>平台：被管设备主动连到一台有公网入口的 relay，' +
          '你在管理台里通过 relay 访问设备，不需要设备有公网 IP、也不需要端口映射。',
      },
      {
        ul: [
          '<b>relay</b>：公网/中心侧。跑隧道接入 + 管理台（HTTP API、WebSocket 终端、前端页面）。',
          '<b>agent</b>：装在设备上。主动连 relay，提供端口转发、文件、服务、串口、诊断、受控执行。',
          '<b>client</b>：装在运维人员机器上。把设备的端口映到本地，或开 SOCKS，也可以直接读写文件仓库。',
          '<b>admin</b>：relay 自带的管理台，浏览器打开 <code>https://relay:9443</code>。',
        ],
      },
      {
        tip: '设备、账号、权限组、令牌、审计、任务都存 relay 的本地 SQLite；每台 relay 是独立的，' +
             '多台 relay 目前只做链路冗余，不做管理面共享。',
      },
    ],
  },
  {
    id: 'deploy',
    title: '2. 部署 relay',
    blocks: [
      {
        h: '方式 A：单二进制（推荐）',
        p: '前端已经编进二进制，不需要另外分发 <code>admin/web</code>，避免“换了 relay 忘了换前端”。',
        code: `cmake -S . -B build_static -DBUILD_SHARED=OFF -DRTUNNEL_ADMIN_WEB=ON
cmake --build build_static --target rtunnel-relay-embedded -j8
# 产物 build_static/bin/rtunnel-relay-embedded，落到目标机后改名/软链为 rtunnel-relay`,
      },
      {
        h: '方式 B：部署包 + 自举',
        p: '包格式统一为 <code>rtunnel-&lt;component&gt;-&lt;ver&gt;-&lt;arch&gt;.tar.gz</code>，' +
          '里面带 <code>install.sh</code>、<code>bin/rtunnel-client</code> 与 <code>rt-bootstrap.sh</code>。',
        code: `# 目标机只要先有一个 rtunnel-client
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \\
  ./rt-bootstrap.sh --component relay --version latest --arch x86_64 --start
# 无 systemd 环境
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log`,
      },
      {
        links: [{ to: '/settings', label: '系统设置' }],
        tip: 'install.sh 只做文件操作：先校验架构、--version 与 SHA256SUMS，再原子切换软链。' +
             '--start/--restart 才会动 systemd；重启后健康检查失败会自动回滚一次。',
      },
    ],
  },
  {
    id: 'device',
    title: '3. 接入设备',
    blocks: [
      {
        p: '设备上装 agent，启动时指定 relay 地址与自己的 devid。agent 会主动连上来，' +
          '之后在「监控 → 设备」里就能看到它。',
        code: `# 最小启动
rtunnel-agent --server relay.example.com --port 18443 --devid box-001

# 常见附加参数
--allow 22,80,443         # 允许转发的端口白名单
--file-dir /opt/data/files # 文件管理根目录
--allow-shell             # 允许管理台开 shell
--allow-tty /dev/ttyS0    # 允许访问的串口
--allow-exec "uptime;df -h;systemctl status *"  # 受控命令白名单（不配则 exec 不可用）`,
      },
      {
        h: '批量升级',
        p: '文件仓库上传包后，在「运维 → OTA」选包、按架构过滤设备、灰度下发。' +
          '失败会按失败率闸门停止，agent 侧有 boot-check 自动回滚。',
        links: [
          { to: '/ota', label: 'OTA 升级' },
          { to: '/distribute', label: '文件分发' },
        ],
      },
      {
        tip: 'agent 只连 relay，不扫描内网、不做设备发现；也不接受 relay 让它去连别的机器。',
        tipType: 'warning',
      },
    ],
  },
  {
    id: 'perm',
    title: '4. 账号、权限组与令牌',
    blocks: [
      {
        p: '权限<b>只来自权限组</b>：账号角色本身不授予任何权限，只决定账号管理页能管谁。' +
          '一个账号/令牌能做什么，取决于它所在组勾了哪些资源键，以及组的设备范围。',
        ul: [
          '<b>权限组</b>：勾资源键（<code>device.view</code>、<code>file.write</code>、<code>device.exec</code>…）' +
            '+ 设备范围（全部设备或指定设备）+ 端口范围。',
          '<b>授权对象</b>：把组绑给控制台账号、接入账号或某个令牌；令牌级分组只认自己绑的组。',
          '<b>接入账号 / 令牌</b>：给外部系统用的 <code>rtk-</code> 令牌，可设时效、流量、并发、带宽。',
          '<b>租户</b>：设备与接入账号的归属边界，令牌只能看到、连到本租户设备；停用租户=停用其下全部账号。',
        ],
      },
      {
        p: '新环境的最小路径：建权限组（勾所需资源 + 设备范围）→ 建接入账号/控制台账号 → ' +
          '在「授权对象」里把组绑给账号 → 签发令牌。',
        links: [
          { to: '/groups', label: '权限组' },
          { to: '/subjects', label: '授权对象' },
          { to: '/access', label: '接入账号' },
          { to: '/tenants', label: '租户' },
        ],
        tip: '令牌不继承运营商侧资源（权限组/字典/中继/证书/设置/审批/模型配置），' +
             '即使被误授也不生效。设备类接口只返回授权范围内的设备。',
      },
    ],
  },
  {
    id: 'daily',
    title: '5. 日常运维',
    blocks: [
      {
        ul: [
          '<b>看状态</b>：「设备」页有 sysinfo、负载、内存、磁盘、网卡、进程列表与最近指标曲线。',
          '<b>连进去</b>：Web 终端（浏览器里直接 SSH）、串口（只读/接管，带录制）。',
          '<b>改文件</b>：设备文件管理（浏览、编辑、Hex、下载上传），仓库文件可分发到设备。',
          '<b>管服务</b>：systemd 单元启停、日志；也可以给进程发信号。',
          '<b>批量</b>：分发任务（灰度、失败率闸门、重试、校验）、OTA 升级。',
          '<b>审计</b>：所有写操作都进审计；「概览」有实时事件流（上下线、会话、审计、告警、任务）。',
        ],
        links: [
          { to: '/agents', label: '设备' },
          { to: '/overview', label: '概览' },
          { to: '/audit', label: '审计' },
        ],
      },
      {
        h: '要“跑一条命令”时',
        p: '用「作业」而不是终端：先 <code>plan</code> 拿到将要执行的内容与 hash，再 <code>apply</code>，' +
          '最后 <code>verify</code>。risk ≥ 2 默认进待审批，<b>必须由另一个账号批准</b>（自己批自己会被拒）。',
        tip: '设备侧必须先配 --allow-exec 白名单，否则 exec 返回 501。白名单只从设备本地配置加载，' +
             '管理面不能远程扩权。',
        links: [{ to: '/operations', label: '作业' }],
      },
      {
        h: '让故障自己处理',
        p: '「自愈」把“告警 → 命令”固化成规则。规则只生成作业，不动手；' +
          '自动执行要三把锁同时成立：规则开 auto_apply、命令风险 ≤ 全局上限（默认 1 只读）、总开关打开。',
        links: [{ to: '/playbooks', label: '自愈规则' }],
      },
    ],
  },
  {
    id: 'ai',
    title: '6. 接入大模型',
    blocks: [
      {
        h: '① 配置模型与密钥',
        p: '在「系统设置 → AI 模型」直接填接口地址与模型名（如 <code>deepseek-v4.1-flash</code>），' +
          '保存密钥，然后点<b>测试连接</b>：它用当前配置发一次最小请求，回来说明耗时、实际路由到的模型名，' +
          '或上游的原始错误（<code>/v1</code> 写错、模型名不对、令牌没权限都能看出来）。' +
          '密钥只写到 relay 本机文件（<code>&lt;admin.db 同目录&gt;/ai.key</code>，0600），不进数据库、不回显。',
        code: `# 也可以用接口
curl -k -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"endpoint":"https://api.deepseek.com/v1","model":"deepseek-v4.1-flash","enabled":true}' \
  https://relay:9443/api/v1/ai/config
curl -k -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"key":"sk-..."}' https://relay:9443/api/v1/ai/key
curl -k -X POST -H "Authorization: Bearer $ATK" https://relay:9443/api/v1/ai/test`,
        links: [{ to: '/settings', label: '去配置' }],
      },
      {
        h: '② 用对话查现状',
        p: '「AI 助手」里可以直接问设备状态、历史指标、谁改了什么、最近的作业与告警。' +
          '模型只能调用<b>只读工具</b>，且工具内部按你自己的权限与设备范围收窄——' +
          '问别人的设备会被拒，和你在页面上点不开是一样的。' +
          '「概览 → AI 诊断」会一次性把离线设备、版本落后、失败作业和告警汇总成排查建议。',
        links: [{ to: '/assistant', label: 'AI 助手' }, { to: '/', label: '概览' }],
      },
      {
        h: '③ 让它动手，但要你点头',
        p: '直接说“把 nginx 重启一下”“清理 /tmp 下的旧包”，模型会先生成一张<b>作业确认卡</b>：' +
          '显示完整命令、风险级别、以及是否需要审批。它<b>不会自己执行</b>——低风险（只读命令）' +
          '点“执行”即可；改状态或破坏性命令要先“批准”，还要走配额与变更窗口。' +
          '也可以让它把这次处理写成<b>自愈规则草稿</b>，创建出来默认停用，你确认后再启用。',
        links: [{ to: '/operations', label: '作业' }, { to: '/playbooks', label: '自愈' }],
      },
      {
        h: '④ 给外部 agent / 脚本用',
        p: '把接口暴露成 MCP 工具：默认只读，写操作要显式开启；调用带 <code>X-Actor</code> 标记进审计。',
        code: `# OpenAPI（构建期从接口文档生成，与路由表对表）
curl -k -sS https://relay:9443/api/v1/openapi.json | head

# MCP server（stdio，只读；加 --allow-write 才暴露写接口）
python3 /opt/rtunnel/relay/deploy/rt-mcp.py --rt https://relay:9443 \\
  --atk-file /etc/rtunnel/atk --insecure`,
      },
      {
        tip: '模型看到的设备内容（进程名、文件、日志）都是不可信输入。不要给它持有终端做自动化，' +
             '也不要让它决定授权；写操作一律走「作业」的审批闸门。',
        tipType: 'warning',
      },
    ],
  },
  {
    id: 'faq',
    title: '7. 常见问题',
    blocks: [
      {
        ul: [
          '<b>设备显示离线</b>：先在设备上 <code>ping</code> relay 的 18443，再看 agent 日志是否在重连；' +
            'relay 侧 <code>grep "agent online"</code> 能看到上线记录。多 relay 时按列表优先级重试，故障地址有 30s 冷却。',
          '<b>接口 401</b>：会话过期或令牌失效；<code>atk-</code> 要放 <code>Authorization: Bearer</code>。',
          '<b>接口 403</b>：缺资源键或设备不在授权范围。去「权限组」补键，或确认设备范围。',
          '<b>exec 返回 501</b>：设备 agent 没配 <code>--allow-exec</code>，或命令不在白名单里。',
          '<b>页面是旧的</b>：换 relay 时没换前端。用内嵌版二进制，或同时更新 <code>admin/web</code>。',
          '<b>AI 返回 503</b>：开关没开、密钥没配、或 endpoint/model 不全；配置页有状态提示。',
        ],
      },
    ],
  },
]

function go(id) {
  const el = document.getElementById(id)
  if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text)
    ElMessage.success('已复制')
  } catch (e) {
    ElMessage.warning('复制失败，请手动选中')
  }
}
</script>

<style scoped>
.guide { max-width: 1000px; }
.lead { margin-bottom: 10px; line-height: 1.7; }
.toc { display: flex; flex-wrap: wrap; gap: 8px; }
.toc-item {
  font-size: 12.5px;
  padding: 5px 10px;
  border: 1px solid var(--el-border-color);
  border-radius: 12px;
  cursor: pointer;
}
.toc-item:hover { border-color: var(--el-color-primary); color: var(--el-color-primary); }
.sec { margin-top: 12px; }
.sec-body { padding-top: 4px; }
.blk { margin-bottom: 14px; }
.sub { font-weight: 600; margin: 10px 0 6px; font-size: 13.5px; }
.para { font-size: 13px; line-height: 1.75; margin-bottom: 6px; }
.ul { margin: 4px 0 8px; padding-left: 20px; font-size: 13px; line-height: 1.8; }
.ul li { margin-bottom: 2px; }
.code-wrap { position: relative; }
.code-wrap .copy { position: absolute; top: 4px; right: 6px; }
.tip { margin: 8px 0; }
.links { margin-top: 8px; display: flex; flex-wrap: wrap; gap: 6px; }
</style>
