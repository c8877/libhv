// 管理台 HTTP 接口文档（atk Token / 账号会话通用）。
// 唯一数据源：/docs 页面与「API Token · 用法」抽屉都读这里。
// 新增路由时必须同步本文件，scripts/check-api-docs.mjs 会对表 admin_routes.cpp。

export const GROUPS = [
  { key: 'device', label: '设备与监控' },
  { key: 'fs', label: '文件仓库' },
  { key: 'dist', label: '分发与 OTA' },
  { key: 'perm', label: '权限与接入账号' },
  { key: 'ops', label: '会话、ACL 与指标' },
  { key: 'sys', label: '账号与系统' },
]

// params: [{ name, in, desc }]；in = path|query|body|header
export const DOCS = [
  // ---------------------------------------------------------------- 设备
  {
    group: 'device', method: 'GET', path: '/api/v1/overview', minRole: 'viewer',
    summary: '全局概览：在线 agent/client/会话数、流量、版本',
    note: '绑定权限组的 Token 只统计组内设备（返回 scoped=true）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/overview"`,
    resp: `{ "agents": 3, "clients": 1, "streams": 2, "bytes_in": 812345, "bytes_out": 991234, "version": "1.4.2", "scoped": false }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents', minRole: 'viewer',
    summary: '在线设备列表（按 Token 设备范围过滤）',
    note: '只返回已注册且在线的设备；离线设备不出现',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents"`,
    resp: `[{ "devid": "mp135", "online": true, "version": "1.4.2", "remote_addr": "10.0.0.8:51000", "streams": 0 }]`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid', minRole: 'viewer',
    summary: '设备详情与活动会话',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    note: '设备离线返回 404 {"error":"device not online"}',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/sysinfo', minRole: 'viewer',
    summary: '实时系统信息（CPU/内存/磁盘/网络）',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'mode', in: 'query', desc: 'inc=增量（设备支持时），省略=全量' },
    ],
    note: '最多等待 2s；超时回最近一次缓存',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/sysinfo?mode=inc"`,
    resp: `{ "ts": 1763349000123, "mode": "full", "sysinfo": { "cpu": 12.5, "mem": { "used": 182, "total": 512 } } }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/procs', minRole: 'viewer',
    summary: '进程列表与资源占用',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'sort', in: 'query', desc: 'cpu / mem / pid / start，默认 cpu' },
      { name: 'q', in: 'query', desc: '匹配 PID / 名称 / 命令行 / 用户' },
      { name: 'limit', in: 'query', desc: '条目上限，默认 200，最大 500' },
    ],
    note: 'CPU 为 300ms 双采样的单核百分比；结果按 26KB 预算截断（truncated=true）；agent 过旧返回 501',
    curl: `curl -sS -H "Authorization: Bearer $ATK" \\\n     "$RT/api/v1/agents/mp135/procs?sort=cpu&limit=100"`,
    resp: `{ "ts": 1763349000123, "sample_ms": 300, "op": "list", "summary": { "total": 128, "running": 2, "threads": 231, "load1": 0.12, "mem_total_kb": 496000, "mem_used_kb": 210000 }, "entries": [{ "pid": 812, "user": "root", "comm": "sshd", "state": "S", "state_text": "睡眠", "cpu_pct": 0.3, "rss_kb": 4096, "threads": 1, "cmd": "sshd: root@pts/0", "kernel": false, "start_ms": 1763300000000 }], "truncated": false }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/procs/:pid', minRole: 'viewer',
    summary: '单个进程详情',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'pid', in: 'path', desc: '进程 PID' },
    ],
    note: '返回命令行、可执行文件、工作目录、RSS/VSZ/峰值/Swap、CPU 时间与启动时间',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/procs/812"`,
    resp: `{ "op": "detail", "pid": 812, "ppid": 1, "user": "root", "state": "S", "comm": "sshd", "cmd": "sshd: root@pts/0", "exe": "/usr/sbin/sshd", "cwd": "/", "rss_kb": 4096, "vsz_kb": 15360, "cpu_sec": 1.23, "start_ms": 1763300000000 }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/agents/:devid/procs/signal', minRole: 'manager',
    summary: '向进程发送信号',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'pid', in: 'body', desc: '目标 PID，必须 > 1' },
      { name: 'signal', in: 'body', desc: '信号号 1–64，常用 15=SIGTERM、9=SIGKILL' },
    ],
    note: '需要 device.proc.signal；拒绝 PID 1 与 agent 自身；操作写入审计日志',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"pid":812,"signal":15}' "$RT/api/v1/agents/mp135/procs/signal"`,
    resp: `{ "op": "signal", "ok": true, "pid": 812, "signal": 15 }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/agents/:devid/exec', minRole: 'manager',
    summary: '在设备上执行白名单内的命令（argv，不经过 shell）',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'argv', in: 'body', desc: '字符串数组，逐段匹配 agent 的 --allow-exec 模板' },
      { name: 'cwd', in: 'body', desc: '工作目录，可选' },
      { name: 'timeout_ms', in: 'body', desc: '1000–30000，默认 30000' },
      { name: 'max_bytes', in: 'body', desc: 'stdout+stderr 合计上限，默认 65536，最大 1 MiB' },
    ],
    note: '需要 device.exec；agent 必须用 --allow-exec 配了白名单，否则 501。'
      + '不经过 shell，超时/超字节会强杀子进程并在结果里标 timed_out/truncated；写审计 device.exec',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"argv":["df","-h"],"timeout_ms":5000}' "$RT/api/v1/agents/mp135/exec"`,
    resp: `{ "ok": true, "exit_code": 0, "stdout": "Filesystem Size Used Avail Use%\n...", "stderr": "", "duration_ms": 12, "truncated": false, "timed_out": false }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/operations/plan', minRole: 'manager',
    summary: '为一个写操作生成计划（不做任何设备动作）',
    params: [
      { name: 'kind', in: 'body', desc: '目前只支持 exec' },
      { name: 'devid', in: 'body', desc: '目标设备' },
      { name: 'argv', in: 'body', desc: '要执行的 argv' },
      { name: 'Idempotency-Key', in: 'header', desc: '可选；同键返回同一作业' },
    ],
    note: '需要 device.exec。返回 id 与 plan_hash；apply 必须回传同一个 hash。risk 1=只读 2=改状态 3=破坏性',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"kind":"exec","devid":"mp135","argv":["systemctl","restart","nginx"]}' "$RT/api/v1/operations/plan"`,
    resp: `{ "id": 7, "status": "planned", "risk": 2, "plan_hash": "9f2c…", "plan": { "kind": "exec", "devid": "mp135", "argv": ["systemctl","restart","nginx"] } }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/operations/:id/apply', minRole: 'manager',
    summary: '执行计划（plan_hash 必须匹配）',
    params: [
      { name: 'id', in: 'path', desc: '作业 id' },
      { name: 'plan_hash', in: 'body', desc: 'plan 返回的 hash' },
      { name: 'Idempotency-Key', in: 'header', desc: '可选；同 actor+key 只执行一次' },
    ],
    note: '需要 device.exec。状态 applied/failed 后重复 apply 直接返回原结果，不会重跑命令',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"plan_hash":"9f2c…"}' "$RT/api/v1/operations/7/apply"`,
    resp: `{ "id": 7, "status": "applied", "result": { "ok": true, "exit_code": 0, "stdout": "" } }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/operations/:id/verify', minRole: 'manager',
    summary: '复核作业结果（可带探针命令）',
    params: [
      { name: 'id', in: 'path', desc: '作业 id' },
      { name: 'argv', in: 'body', desc: '可选探针命令；不传则以 apply 结果判定' },
    ],
    note: '需要 device.exec。带 argv 时探针 exit_code=0 才算通过；结果写审计 operation.verify',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"argv":["systemctl","is-active","nginx"]}' "$RT/api/v1/operations/7/verify"`,
    resp: `{ "id": 7, "verified": true, "probe": { "ok": true, "exit_code": 0, "stdout": "active\n" } }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/operations/:id/approve', minRole: 'manager',
    summary: '批准或拒绝待审批的作业',
    params: [
      { name: 'id', in: 'path', desc: '作业 id' },
      { name: 'decision', in: 'body', desc: 'approve 或 reject' },
      { name: 'note', in: 'body', desc: '备注 / 拒绝原因，可选（≤300 字）' },
    ],
    note: '需要 op.approve（接入令牌不生效）。risk ≥ ops.require_approval_risk 的作业落库即待审批，'
      + '必须由另一个账号批准：自己批自己返回 403。批准后 status=approved，可继续 apply',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\\
     -d '{"decision":"approve","note":"已确认"}' "$RT/api/v1/operations/7/approve"`,
    resp: `{ "id": 7, "status": "approved", "approver": "ops#2/manager/10.0.0.9", "approved_ms": 1764192000000 }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/operations', minRole: 'viewer',
    summary: '操作作业列表',
    params: [
      { name: 'devid', in: 'query', desc: '只看某台设备' },
      { name: 'limit', in: 'query', desc: '默认 50，最大 500' },
    ],
    note: '需要 device.view；受限账号只能看到授权范围内设备的作业',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/operations?devid=mp135&limit=20"`,
    resp: `{ "items": [{ "id": 7, "kind": "exec", "target_id": "mp135", "status": "applied", "risk": 2 }] }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/playbooks', minRole: 'manager',
    summary: '自愈规则列表',
    note: '需要 playbook.read（接入令牌不生效）。规则只做一件事：告警匹配后生成一个 operation，'
      + '是否自动执行由规则开关 + playbook.auto_apply_max_risk 共同决定。',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/playbooks"`,
    resp: `{ "items": [{ "id": 1, "name": "上线自检", "event": "device.online", "devid_glob": "dev*", "argv": ["uptime"], "verify_argv": ["uptime"], "risk": 1, "auto_apply": true, "cooldown_s": 300, "max_per_hour": 6, "enabled": true }] }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/playbooks/:id', minRole: 'manager',
    summary: '自愈规则详情',
    params: [{ name: 'id', in: 'path', desc: '规则 ID' }],
    note: '需要 playbook.read',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/playbooks/1"`,
    resp: `{ "id": 1, "name": "上线自检", "event": "device.online", "argv": ["uptime"], "risk": 1 }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/playbooks', minRole: 'manager',
    summary: '新建自愈规则',
    params: [
      { name: 'name', in: 'body', desc: '唯一名称，1-64 字节' },
      { name: 'event', in: 'body', desc: 'device.online | device.offline | task.failed | cert.expiring | *' },
      { name: 'devid_glob', in: 'body', desc: '设备 glob（* ?），默认 *' },
      { name: 'argv', in: 'body', desc: '命令 argv 数组（不经过 shell，需在 agent 白名单内）' },
      { name: 'verify_argv', in: 'body', desc: '可选复核 argv，exit_code=0 才算自愈成功' },
      { name: 'auto_apply', in: 'body', desc: '是否自动执行（仍受全局风险上限约束）' },
      { name: 'cooldown_s', in: 'body', desc: '同规则冷却秒数，默认 300' },
      { name: 'max_per_hour', in: 'body', desc: '每规则每小时上限，默认 6' },
    ],
    note: '需要 playbook.write。risk 由服务端按 argv 计算，客户端传 risk 会被忽略——'
      + '否则可以把「重启服务」标成只读来绕过审批',
    body: `{ "name": "上线自检", "event": "device.online", "devid_glob": "dev*", "argv": ["uptime"], "auto_apply": true }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"name":"上线自检","event":"device.online","argv":["uptime"],"auto_apply":true}' "$RT/api/v1/playbooks"`,
    resp: `{ "id": 1, "name": "上线自检", "risk": 1, "auto_apply": true, "enabled": true }`,
  },
  {
    group: 'device', method: 'PUT', path: '/api/v1/playbooks/:id', minRole: 'manager',
    summary: '修改自愈规则',
    params: [
      { name: 'id', in: 'path', desc: '规则 ID' },
      { name: 'argv', in: 'body', desc: '新的 argv（risk 重新计算）' },
    ],
    note: '需要 playbook.write。字段与新建相同，未传的用默认值（不是部分更新）',
    body: `{ "name": "上线自检", "event": "device.online", "argv": ["uptime"], "auto_apply": false }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"name":"上线自检","event":"device.online","argv":["uptime"],"auto_apply":false}' "$RT/api/v1/playbooks/1"`,
    resp: `{ "id": 1, "auto_apply": false }`,
  },
  {
    group: 'device', method: 'DELETE', path: '/api/v1/playbooks/:id', minRole: 'manager',
    summary: '删除自愈规则',
    params: [{ name: 'id', in: 'path', desc: '规则 ID' }],
    note: '需要 playbook.write。只删规则，历史触发记录（operations）保留',
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/playbooks/1"`,
    resp: `{ "ok": true }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/playbooks/:id/enable', minRole: 'manager',
    summary: '启用 / 停用自愈规则',
    params: [
      { name: 'id', in: 'path', desc: '规则 ID' },
      { name: 'enabled', in: 'body', desc: 'true 启用，false 停用' },
    ],
    note: '需要 playbook.write',
    body: `{ "enabled": false }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"enabled":false}' "$RT/api/v1/playbooks/1/enable"`,
    resp: `{ "id": 1, "enabled": false }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/playbooks/:id/run', minRole: 'manager',
    summary: '按规则给指定设备生成作业',
    params: [
      { name: 'id', in: 'path', desc: '规则 ID' },
      { name: 'devid', in: 'body', desc: '目标设备（必须在授权范围内）' },
    ],
    note: '需要 playbook.write。与自动触发走同一路径：auto_apply 且风险不超上限才直接执行，'
      + '否则生成待审批作业',
    body: `{ "devid": "mp135" }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"devid":"mp135"}' "$RT/api/v1/playbooks/1/run"`,
    resp: `{ "id": 12, "actor": "playbook:1:上线自检", "status": "verified", "risk": 1 }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/playbooks/fires', minRole: 'manager',
    summary: '自愈触发记录',
    params: [{ name: 'limit', in: 'query', desc: '默认 50，最大 500' }],
    note: '需要 playbook.read。受限账号只看得到授权设备上的记录；记录来自 operations 里 '
      + 'actor_type=playbook 的作业',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/playbooks/fires?limit=20"`,
    resp: `{ "items": [{ "op_id": 12, "playbook_id": 1, "playbook": "上线自检", "devid": "mp135", "status": "verified", "created_ms": 1764136800000 }] }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/ai/config', minRole: 'operator',
    summary: '本账号的模型配置（不含密钥内容）',
    note: '需要 ai.chat。配置按账号隔离：每个账号一份，别人的配置和密钥你看不到也用不到。'
      + '返回 provider/model/endpoint/key_set/ready，以及 providers 预置清单（前端下拉用）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/config"`,
    resp: `{ "subject": "u-3", "provider": "deepseek", "endpoint": "https://api.deepseek.com/v1", "model": "deepseek-chat", "key_set": true, "key_hint": "sk-…abcd", "ready": true, "insecure": false, "tools": 9, "providers": [{ "key": "deepseek", "label": "DeepSeek", "endpoint": "https://api.deepseek.com/v1", "model": "deepseek-chat" }] }`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/ai/config', minRole: 'operator',
    summary: '保存本账号的模型配置',
    params: [
      { name: 'provider', in: 'body', desc: 'deepseek / moonshot / openai / qwen / zhipu / ollama / custom' },
      { name: 'model', in: 'body', desc: '模型名；不传时用该服务商的默认值' },
      { name: 'endpoint', in: 'body', desc: '可选：覆盖服务商默认地址（自定义网关时必填）' },
      { name: 'insecure', in: 'body', desc: '可选：跳过 TLS 证书校验（内网自签）' },
    ],
    note: '需要 ai.chat。只影响调用者自己的账号；采样参数（max_tokens/temperature/超时/轮次）'
      + '固定在服务端，不作为配置项暴露',
    body: `{ "provider": "deepseek", "model": "deepseek-chat" }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
     -d '{"provider":"deepseek","model":"deepseek-chat"}' "$RT/api/v1/ai/config"`,
    resp: `{ "subject": "u-3", "provider": "deepseek", "model": "deepseek-chat", "key_set": true, "ready": true }`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/ai/key', minRole: 'operator',
    summary: '保存（或替换）本账号的模型 API Key',
    params: [{ name: 'key', in: 'body', desc: '密钥明文，1-512 字节，不能含空白' }],
    note: '需要 ai.chat。写到 relay 本机 <admin.db 同目录>/ai/<subject>.key（0600），'
      + '不进数据库、不回显；审计只记前后各几位',
    body: `{ "key": "sk-..." }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
     -d '{"key":"sk-..."}' "$RT/api/v1/ai/key"`,
    resp: `{ "key_set": true, "key_hint": "sk-…abcd", "ready": true }`,
  },
  {
    group: 'sys', method: 'DELETE', path: '/api/v1/ai/key', minRole: 'operator',
    summary: '清除本账号的模型密钥',
    note: '需要 ai.chat。清除后 /ai/chat 返回 503 并提示去配置，模型名与服务商保留',
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/key"`,
    resp: `{ "key_set": false, "ready": false }`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/ai/chat', minRole: 'operator',
    summary: '与模型对话（只读工具 + 提议作业，OpenAI 兼容 function calling）',
    params: [
      { name: 'messages', in: 'body', desc: '[{role: user|assistant, content}]，只接受这两种角色' },
      { name: 'devid', in: 'body', desc: '可选：当前设备上下文，必须在授权范围内' },
      { name: 'session_id', in: 'body', desc: '可选：续在已有会话上；0/缺省则自动新建并返回 id' },
    ],
    note: '需要 ai.chat，且本账号已配好 provider/model/key。每轮问答都会写入会话记录（永久保留）。模型只能调用只读工具'
      + '（设备/指标/审计/作业/用量/自愈），工具内部按调用者的资源权限与设备范围收窄。'
      + '客户端传的 system 消息会被丢弃；写操作不会被执行：模型用 propose_exec 生成待确认作业、'
      + 'draft_playbook 生成规则草稿，在 actions 里返回，由页面渲染成确认卡片',
    body: `{ "messages": [{ "role": "user", "content": "现在有哪些设备在线？" }] }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
     -d '{"messages":[{"role":"user","content":"现在有哪些设备在线？"}]}' "$RT/api/v1/ai/chat"`,
    resp: `{ "reply": "共 3 台设备，在线 2 台……", "actions": [], "tools": [{ "name": "list_devices", "args": {}, "ok": true }], "rounds": 2 }`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/ai/test', minRole: 'operator',
    summary: '测试本账号的模型连通性（发一次最小请求）',
    note: '需要 ai.chat 且本账号配置完整。返回耗时与实际模型名；上游错误原文一并返回，'
      + '便于排查 /v1 前缀、模型名、令牌权限',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/test"`,
    resp: `{ "ok": true, "provider": "deepseek", "endpoint": "https://api.deepseek.com/v1", "model": "deepseek-chat", "latency_ms": 860, "reply": "正常", "served_model": "deepseek-chat" }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/ai/sessions', minRole: 'operator',
    summary: '本账号的历史会话（最近 10 个，可翻页）',
    params: [
      { name: 'limit', in: 'query', desc: '默认 10，最大 100' },
      { name: 'before', in: 'query', desc: '翻页游标：上一页的 next_before_ms' },
      { name: 'before_id', in: 'query', desc: '翻页游标：上一页的 next_before_id' },
    ],
    note: '需要 ai.chat。会话按账号隔离、**永久保留**（没有过期清理，只能主动删除）。'
      + '列表按最后活动时间倒序；has_more 为真时用 next_before_ms/next_before_id 继续翻',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/sessions?limit=10"`,
    resp: `{ "items": [{ "id": 12, "title": "mp135 掉线的排查", "msg_count": 6, "created_ms": 1764136800000, "updated_ms": 1764137400000 }], "has_more": true, "next_before_ms": 1764137400000, "next_before_id": 12 }`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/ai/sessions', minRole: 'operator',
    summary: '显式新建一个空会话',
    params: [{ name: 'title', in: 'body', desc: '可选；不给时第一句话会自动成为标题' }],
    note: '需要 ai.chat。一般不用手动建：带 session_id=0 提问会自动建一个并返回 id',
    body: `{ "title": "新会话" }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
     -d '{"title":"新会话"}' "$RT/api/v1/ai/sessions"`,
    resp: `{ "id": 13, "title": "新会话", "msg_count": 0 }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/ai/sessions/:id', minRole: 'operator',
    summary: '读取一个会话的全部消息',
    params: [{ name: 'id', in: 'path', desc: '会话 id' }],
    note: '需要 ai.chat。只能读自己的会话，别人的 id 返回 404。'
      + 'assistant 消息里还原了 actions / tools / meta / error，可以直接渲染成原来的样子',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/sessions/12"`,
    resp: `{ "id": 12, "title": "mp135 掉线的排查", "msg_count": 2, "messages": [{ "role": "user", "content": "mp135 为什么老掉线" }, { "role": "assistant", "content": "…", "tools": [{ "name": "device_detail", "ok": true }], "meta": "2 轮 · 96 tokens" }] }`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/ai/sessions/:id', minRole: 'operator',
    summary: '给会话改名',
    params: [
      { name: 'id', in: 'path', desc: '会话 id' },
      { name: 'title', in: 'body', desc: '新标题，1-80 字节' },
    ],
    note: '需要 ai.chat。只能改自己的会话',
    body: `{ "title": "mp135 掉线排查" }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
     -d '{"title":"mp135 掉线排查"}' "$RT/api/v1/ai/sessions/12"`,
    resp: `{ "id": 12, "title": "mp135 掉线排查" }`,
  },
  {
    group: 'sys', method: 'DELETE', path: '/api/v1/ai/sessions/:id', minRole: 'operator',
    summary: '删除会话（含全部消息）',
    params: [{ name: 'id', in: 'path', desc: '会话 id' }],
    note: '需要 ai.chat。只能删自己的会话；删除不可恢复，其它会话不受影响',
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/ai/sessions/12"`,
    resp: `{ "ok": true }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/metrics/history', minRole: 'viewer',
    summary: '分钟级指标历史（relay 采样）',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'from', in: 'query', desc: '起始时间（毫秒或秒）；默认 to-6h' },
      { name: 'to', in: 'query', desc: '结束时间；默认现在' },
      { name: 'step', in: 'query', desc: '聚合步长（秒，按 60 对齐），默认 60；区间最大 31 天' },
    ],
    note: '需要 device.view 且设备在授权范围内。relay 每分钟给在线设备要一份增量 sysinfo 落一条，'
      + '保留 metrics.retention_days（默认 7 天）。step>60 按桶取平均，rx/tx 取桶内最大值',
    curl: `curl -sS -H "Authorization: Bearer $ATK" \\\\
  "$RT/api/v1/agents/mp135/metrics/history?from=1764115200000&to=1764136800000&step=300"`,
    resp: `{ "devid": "mp135", "from": 1764115200000, "to": 1764136800000, "step": 300, "points": [{ "ts": 1764115500000, "cpu": 12.4, "mem": 412000, "mem_total": 1048576, "load1": 0.31, "rx": 918273645, "tx": 12345678, "samples": 5 }] }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/events', minRole: 'viewer',
    summary: '事件流（SSE）：设备上下线、会话、审计、告警、任务',
    params: [
      { name: 'last', in: 'query', desc: '从该 seq 之后开始（也可用 Last-Event-ID 头）' },
    ],
    note: '需要 device.view。响应是 text/event-stream：先一条 hello（带 cursor），之后每条事件带 id/event/data，'
      + '15s 一次心跳注释。受限账号只收自己设备的事件；游标过期会收到 bus.lost 并跳到最旧一条。'
      + '断线重连带 Last-Event-ID 即可续传',
    curl: `curl -N -H "Authorization: Bearer $ATK" "$RT/api/v1/events"`,
    resp: `event: hello\\ndata: {"cursor":42,"restricted":false}\\n\\nevent: device.online\\nid: 43\\ndata: {"devid":"mp135","version":"1.4.137","ts":1764136800000}\\n\\n`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/net', minRole: 'viewer',
    summary: '网络与 IPC 快照（TCP/UDP/UNIX/IPC）',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'scope', in: 'query', desc: 'tcp / udp / unix / ipc / all，默认 tcp' },
      { name: 'proc', in: 'query', desc: '1=反查占用进程（默认），0=关闭' },
      { name: 'refresh', in: 'query', desc: '1=绕过 relay 3s 缓存' },
      { name: 'limit', in: 'query', desc: '每类条目上限，默认 200，最大 500' },
    ],
    note: '设备离线时返回最后一次快照（cached=true）；agent < 1.4.28 返回 501',
    curl: `curl -sS -H "Authorization: Bearer $ATK" \\\n     "$RT/api/v1/agents/mp135/net?scope=tcp&proc=1"`,
    resp: `{ "ts": 1763349000123, "scope": "tcp", "cached": false, "summary": { "tcp": 12, "udp": 6, "unix": 41, "ipc": 3 }, "tcp": { "total": 12, "listening": 5, "connected": 6, "entries": [{ "local_addr": "0.0.0.0", "local_port": 22, "remote_addr": "0.0.0.0", "remote_port": 0, "state": "LISTEN", "pid": 812, "comm": "sshd" }] } }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/storage', minRole: 'viewer',
    summary: '分区与目录占用（按挂载点/路径/层级）',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'mount', in: 'query', desc: '挂载点，默认 /' },
      { name: 'path', in: 'query', desc: '相对挂载点的子目录，省略=列分区根' },
      { name: 'depth', in: 'query', desc: '相对 path 的回传层级，默认 1，最大 64' },
      { name: 'force', in: 'query', desc: '1=跳过 60s 缓存重新扫描' },
    ],
    note: 'path 必须落在 mount 内；同一参数 60s 内复用上次结果，force=1 重扫；大目录返回 gzip（按 Accept-Encoding），最多等 30s',
    curl: `curl -sS --compressed -H "Authorization: Bearer $ATK" \\\n     "$RT/api/v1/agents/mp135/storage?mount=/&depth=1"`,
    resp: `{ "ok": true, "mount": "/", "total_kb": 7456000, "used_kb": 2310000, "nodes": [{ "name": "root", "path": "/", "size_kb": 2310000, "children": 18 }] }`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/agents/:devid/power', minRole: 'manager',
    summary: '设备重启 / 关机',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'action', in: 'body', desc: 'reboot 或 poweroff' },
      { name: 'password', in: 'body', desc: '设备 root 密码，agent 侧校验，不落日志' },
      { name: 'delay_ms', in: 'body', desc: '延迟毫秒，默认 1000，最大 60000' },
    ],
    note: '需要 device.power；操作写审计；密码只透传不记录',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n     -d '{"action":"reboot","password":"***","delay_ms":1000}' "$RT/api/v1/agents/mp135/power"`,
    resp: `{ "ok": true, "action": "reboot", "delay_ms": 1000 }`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/diag', minRole: 'manager',
    summary: '下载诊断包（日志 + sysinfo）',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    note: '返回 application/gzip，响应头 X-Diag-Sha256；最多等 30s；需要 device.diag',
    curl: `curl -sS -H "Authorization: Bearer $ATK" -o diag.tar.gz "$RT/api/v1/agents/mp135/diag"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/metrics/series', minRole: 'viewer',
    summary: '读取指标时间序列',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'series', in: 'query', desc: '指标名，如 cpu / mem / net.rx' },
      { name: 'since_ts', in: 'query', desc: '起始时间戳（秒），0=全部' },
    ],
    note: '响应为 gzip，curl 加 --compressed',
    curl: `curl -sS --compressed -H "Authorization: Bearer $ATK" \\\n     "$RT/api/v1/agents/mp135/metrics/series?series=cpu&since_ts=0"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/metrics/series/config', minRole: 'viewer',
    summary: '读取指标采集配置',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/metrics/series/config"`,
  },
  {
    group: 'device', method: 'PUT', path: '/api/v1/agents/:devid/metrics/series/config', minRole: 'manager',
    summary: '写入指标采集配置（整表替换）',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    note: 'body 为数组；interval_sec 1..300，points 60..86400',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '[{"series":"cpu","enabled":true,"interval_sec":5,"points":720}]' \\\n  "$RT/api/v1/agents/mp135/metrics/series/config"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/ttys', minRole: 'viewer',
    summary: '串口配置列表',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/ttys"`,
  },
  {
    group: 'device', method: 'PUT', path: '/api/v1/agents/:devid/ttys', minRole: 'manager',
    summary: '新增或修改串口',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    note: '按 path 唯一；roles 控制可访问角色',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"path":"/dev/ttyS1","alias":"rs485","baud":115200,"data_bits":8,\n       "parity":"N","stop_bits":1,"roles":"operator,manager,admin","enabled":true}' \\\n  "$RT/api/v1/agents/mp135/ttys"`,
  },
  {
    group: 'device', method: 'DELETE', path: '/api/v1/agents/:devid/ttys', minRole: 'manager',
    summary: '删除串口配置',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'path', in: 'query', desc: '串口路径，需 URL 编码' },
    ],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" \\\n  "$RT/api/v1/agents/mp135/ttys?path=%2Fdev%2FttyS1"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/agents/:devid/ttys/discovered', minRole: 'manager',
    summary: '探测设备上可用的串口',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/ttys/discovered"`,
  },
  {
    group: 'device', method: 'GET', path: '/api/v1/ttylogs/:name', minRole: 'manager',
    summary: '下载串口日志文件',
    params: [{ name: 'name', in: 'path', desc: '日志文件名' }],
    curl: `curl -sS -OJ -H "Authorization: Bearer $ATK" "$RT/api/v1/ttylogs/ttyS1-20261117.log"`,
  },
  {
    group: 'device', method: 'PUT', path: '/api/v1/agents/:devid/caps', minRole: 'manager',
    summary: '下发设备能力配置（文件/服务/串口/OTA/Shell）',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    note: '能力字段需 device.caps.write，ota 需 device.ota.write，allow_tty 需 device.tty.write；设备需在线并在 2s 内应答',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ports":[22,8080],"fs":true,"fs_root":"/","fs_rw":true,"fs_max_mb":512,\n       "svc":true,"svc_rw":true,"ota":true,"allow_tty":["/dev/ttyS0"]}' \\\n  "$RT/api/v1/agents/mp135/caps"`,
  },
  {
    group: 'device', method: 'POST', path: '/api/v1/agents/:devid/kick', minRole: 'manager',
    summary: '把设备踢下线（设备会自动重连）',
    params: [{ name: 'devid', in: 'path', desc: '设备 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/agents/mp135/kick"`,
  },
  {
    group: 'device', method: 'GET', path: '/device/:devid/:port/*', minRole: 'operator',
    summary: '反向代理到设备上的 HTTP 服务',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'port', in: 'path', desc: '设备端口，需在 caps.ports 中' },
      { name: '*', in: 'path', desc: '转发到上游的路径' },
    ],
    note: '支持 GET/POST/PUT/DELETE/HEAD/OPTIONS；不支持 WebSocket；并发上限 proxy.max_concurrent（默认 32）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/device/mp135/80/api/status"`,
  },

  // ---------------------------------------------------------------- 文件仓库
  {
    group: 'fs', method: 'GET', path: '/api/v1/files', minRole: 'operator',
    summary: '仓库列表：目录 + 文件（含 sha256）',
    params: [
      { name: 'dir', in: 'query', desc: '相对仓库根的目录，如 agent 或 firmware/mp135' },
      { name: 'sort', in: 'query', desc: 'name | size | mtime，默认 name；目录始终排在前' },
      { name: 'order', in: 'query', desc: 'asc | desc' },
      { name: 'recursive', in: 'query', desc: '=1 配合 q 递归搜索子目录' },
      { name: 'q', in: 'query', desc: '按名称过滤，最多返回 500 条' },
    ],
    note: '需要 relay 启动参数 --admin-file-dir；recursive 模式不返回 sha256',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/files?dir=agent"\ncurl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/files?dir=agent&recursive=1&q=lite"`,
    resp: `[{ "name": "rtunnel-agent-lite-1.4.3-x86_64.tar.gz", "type": "file", "size": 240372, "mtime_ms": 1763340000000, "sha256": "9f2c…" }]`,
  },
  {
    group: 'fs', method: 'GET', path: '/api/v1/files/:name', minRole: 'operator',
    summary: '下载文件，支持单段 Range 断点续传',
    params: [
      { name: 'name', in: 'path', desc: '文件名，不含 /' },
      { name: 'dir', in: 'query', desc: '文件所在子目录，默认根目录' },
      { name: 'Range', in: 'header', desc: 'bytes=start-end 或 bytes=-suffix' },
    ],
    note: '默认上限 file.max_download_mb=32',
    curl: `curl -sS -OJ -H "Authorization: Bearer $ATK" "$RT/api/v1/files/fw.bin?dir=firmware"\ncurl -sS -H "Authorization: Bearer $ATK" -H 'Range: bytes=0-1048575' -o part0 "$RT/api/v1/files/fw.bin?dir=firmware"`,
  },
  {
    group: 'fs', method: 'POST', path: '/api/v1/files/:name', minRole: 'operator',
    summary: '上传文件（整包/分片），或 action=mkdir / move',
    params: [
      { name: 'name', in: 'path', desc: '文件名；action=mkdir 时是目录名' },
      { name: 'dir', in: 'query', desc: '目标子目录，默认根目录' },
      { name: 'action', in: 'query', desc: 'mkdir 新建目录；move 移动/改名' },
      { name: 'to', in: 'query', desc: 'action=move 时的目标相对路径（含新文件名）' },
      { name: 'upload', in: 'query', desc: '分片上传 ID（字母数字 _-，≤64）' },
      { name: 'append', in: 'query', desc: '=1 追加到临时文件' },
      { name: 'offset', in: 'query', desc: '已收字节数，必须等于服务端当前大小' },
      { name: 'finalize', in: 'query', desc: '=1 落盘并计算 sha256' },
    ],
    note: '默认上限 file.max_upload_mb=32；分片 offset 不匹配返回 409；目录已存在时 mkdir 返回 409',
    curl: `# 整包上传到 agent/\ncurl -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @rtunnel-agent-lite-1.4.3-x86_64.tar.gz \\\n  "$RT/api/v1/files/rtunnel-agent-lite-1.4.3-x86_64.tar.gz?dir=agent"\n\n# 建目录\ncurl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/files/agent?action=mkdir"\n\n# 移动 / 改名：agent/a.bin -> agent/old/a.bin\ncurl -sS -X POST -H "Authorization: Bearer $ATK" \\\n  "$RT/api/v1/files/a.bin?action=move&dir=agent&to=agent/old/a.bin"\n\n# 分片\ncurl -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @c0 \\\n  "$RT/api/v1/files/fw.bin?dir=firmware&upload=u1&append=1&offset=0"\ncurl -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @c1 \\\n  "$RT/api/v1/files/fw.bin?dir=firmware&upload=u1&append=1&offset=1048576"\ncurl -sS -X POST -H "Authorization: Bearer $ATK" \\\n  "$RT/api/v1/files/fw.bin?dir=firmware&upload=u1&finalize=1"`,
  },
  {
    group: 'fs', method: 'GET', path: '/api/v1/uploads/:name', minRole: 'operator',
    summary: '查询分片上传进度',
    params: [
      { name: 'name', in: 'path', desc: '文件名' },
      { name: 'dir', in: 'query', desc: '文件所在子目录' },
      { name: 'upload', in: 'query', desc: '分片上传 ID' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/uploads/fw.bin?dir=firmware&upload=u1"`,
    resp: `{ "received": 2097152 }`,
  },
  {
    group: 'fs', method: 'PUT', path: '/api/v1/files/:name', minRole: 'manager',
    summary: '整文件替换（文本编辑保存）',
    params: [
      { name: 'name', in: 'path', desc: '文件名' },
      { name: 'dir', in: 'query', desc: '文件所在子目录' },
    ],
    note: '默认上限 file.max_edit_mb=4；原子写（临时文件 + rename）',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" --data-binary @config.json \\\n  "$RT/api/v1/files/config.json?dir=firmware"`,
  },
  {
    group: 'fs', method: 'DELETE', path: '/api/v1/files/:name', minRole: 'manager',
    summary: '删除仓库文件或目录',
    params: [
      { name: 'name', in: 'path', desc: '文件名或目录名' },
      { name: 'dir', in: 'query', desc: '所在父目录' },
      { name: 'recursive', in: 'query', desc: '删除非空目录时必须为 1' },
    ],
    note: '非空目录不带 recursive=1 返回 409',
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/files/agent?dir=&recursive=1"`,
  },

  // ---------------------------------------------------------------- 分发
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions', minRole: 'manager',
    summary: '创建分发任务（普通文件或 OTA）',
    note: 'devids 1..200；file 跳过离线设备，ota 可排队；时间窗最长 30 天',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"file":"fw.bin","devids":["mp135","mp136"],"dir":"firmware",\n       "kind":"file","force":false,"max_retries":3}' \\\n  "$RT/api/v1/distributions"\n\n# OTA（升级包放在 agent/ 目录，文件名带架构）\ncurl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"file":"agent/rtunnel-agent-lite-1.4.3-x86_64.tar.gz","devids":["mp135"],"kind":"ota","version":"1.4.3"}' \\\n  "$RT/api/v1/distributions"`,
  },
  {
    group: 'dist', method: 'GET', path: '/api/v1/distributions', minRole: 'manager',
    summary: '分发任务列表',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions"`,
  },
  {
    group: 'dist', method: 'GET', path: '/api/v1/distributions/summary', minRole: 'manager',
    summary: '分发任务汇总（按状态计数）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/summary"`,
  },
  {
    group: 'dist', method: 'GET', path: '/api/v1/distributions/:id', minRole: 'manager',
    summary: '分发任务详情（每台设备的进度）',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/12"`,
  },
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions/:id/retry', minRole: 'manager',
    summary: '重试失败设备',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/12/retry"`,
  },
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions/:id/cancel', minRole: 'manager',
    summary: '取消任务',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/12/cancel"`,
  },
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions/:id/verify', minRole: 'manager',
    summary: '校验冲突设备（目标文件已存在时确认覆盖）',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/12/verify"`,
  },
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions/:id/force', minRole: 'manager',
    summary: '强制覆盖冲突设备',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/distributions/12/force"`,
  },
  {
    group: 'dist', method: 'POST', path: '/api/v1/distributions/:id/extend', minRole: 'manager',
    summary: '延长超时任务的截止时间',
    params: [{ name: 'id', in: 'path', desc: '任务 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"deadline_at_ms":1763435400000}' "$RT/api/v1/distributions/12/extend"`,
  },

  // ---------------------------------------------------------------- 权限
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/dict/ports', minRole: 'operator',
    summary: '端口字典',
    params: [{ name: 'q', in: 'query', desc: '搜索关键词' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/dict/ports?q=80"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ports', minRole: 'manager',
    summary: '新增端口字典项',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"value":"8080","name":"HTTP 备用","description":"调试端口"}' \\\n  "$RT/api/v1/permission/dict/ports"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/dict/ports/:id', minRole: 'manager',
    summary: '修改端口字典项',
    params: [{ name: 'id', in: 'path', desc: '字典项 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"HTTP 备用","description":"调试","enabled":true}' \\\n  "$RT/api/v1/permission/dict/ports/7"`,
  },
  {
    group: 'perm', method: 'DELETE', path: '/api/v1/permission/dict/ports/:id', minRole: 'manager',
    summary: '删除端口字典项（被引用时 409）',
    params: [{ name: 'id', in: 'path', desc: '字典项 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/dict/ports/7"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ports/batch', minRole: 'manager',
    summary: '批量新增端口',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"values":["80","443","8080"],"name_prefix":"常用"}' \\\n  "$RT/api/v1/permission/dict/ports/batch"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ports/batch-delete', minRole: 'manager',
    summary: '批量删除端口',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ids":[7,8]}' "$RT/api/v1/permission/dict/ports/batch-delete"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/dict/ttys', minRole: 'operator',
    summary: '串口字典',
    params: [{ name: 'q', in: 'query', desc: '搜索关键词' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/dict/ttys?q=ttyS"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ttys', minRole: 'manager',
    summary: '新增串口字典项',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"value":"/dev/ttyS1","name":"RS485","description":"产线总线"}' \\\n  "$RT/api/v1/permission/dict/ttys"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/dict/ttys/:id', minRole: 'manager',
    summary: '修改串口字典项',
    params: [{ name: 'id', in: 'path', desc: '字典项 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"RS485","description":"产线总线","enabled":true}' \\\n  "$RT/api/v1/permission/dict/ttys/3"`,
  },
  {
    group: 'perm', method: 'DELETE', path: '/api/v1/permission/dict/ttys/:id', minRole: 'manager',
    summary: '删除串口字典项（被引用时 409）',
    params: [{ name: 'id', in: 'path', desc: '字典项 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/dict/ttys/3"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ttys/batch', minRole: 'manager',
    summary: '批量新增串口',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"values":["/dev/ttyS1","/dev/ttyS2"],"name_prefix":"串口"}' \\\n  "$RT/api/v1/permission/dict/ttys/batch"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/dict/ttys/batch-delete', minRole: 'manager',
    summary: '批量删除串口',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ids":[3,4]}' "$RT/api/v1/permission/dict/ttys/batch-delete"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/client-accounts', minRole: 'operator',
    summary: '接入账号列表',
    params: [{ name: 'q', in: 'query', desc: '搜索用户名' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts?q=box"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/client-accounts/:id', minRole: 'operator',
    summary: '接入账号详情',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/client-accounts/:id/effective', minRole: 'operator',
    summary: '接入账号生效权限',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1/effective"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/client-accounts/:id/tokens', minRole: 'operator',
    summary: '接入账号的 Token 列表',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1/tokens"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/client-accounts', minRole: 'manager',
    summary: '新建接入账号',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"username":"box02","display_name":"产线 B","note":"2026 新增"}' \\\n  "$RT/api/v1/client-accounts"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/client-accounts/:id', minRole: 'manager',
    summary: '修改接入账号（不含用户名）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"display_name":"产线 B","note":"","status":"active"}' \\\n  "$RT/api/v1/client-accounts/1"`,
  },
  {
    group: 'perm', method: 'DELETE', path: '/api/v1/client-accounts/:id', minRole: 'admin',
    summary: '删除接入账号',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/client-accounts/:id/kick', minRole: 'manager',
    summary: '断开该接入账号的全部在线会话（令牌吊销/停用、权限组变更时自动执行）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1/kick"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/client-accounts/:id/tokens', minRole: 'manager',
    summary: '为接入账号创建 Token（配额/限速在这里）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    note: 'rate_per_min/burst/max_sessions/rate_kbps/quota_gb/quota_period/quota_action 可选；'
      + 'rate_kbps 是带宽上限（kilobit/s，0 不限），双向合计计量',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"box02-main","expires_in_days":180,"rate_per_min":600,"burst":100,\n       "max_sessions":16,"quota_gb":50,"quota_period":"month","quota_action":"block"}' \\\n  "$RT/api/v1/client-accounts/1/tokens"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/client-tokens/:id', minRole: 'manager',
    summary: '修改接入 Token（未传字段保持原值）',
    params: [{ name: 'id', in: 'path', desc: 'Token ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"rate_per_min":1200,"rate_kbps":20000,"status":"active"}' "$RT/api/v1/client-tokens/7"`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/client-tokens/:id/rotate', minRole: 'manager',
    summary: '轮换接入 Token',
    params: [{ name: 'id', in: 'path', desc: 'Token ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/client-tokens/7/rotate"`,
  },
  {
    group: 'perm', method: 'DELETE', path: '/api/v1/client-tokens/:id', minRole: 'manager',
    summary: '吊销接入 Token',
    params: [{ name: 'id', in: 'path', desc: 'Token ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/client-tokens/7"`,
  },

  // ---------------------------------------------------------------- 会话/ACL/指标
  {
    group: 'ops', method: 'GET', path: '/api/v1/streams', minRole: 'operator',
    summary: '活动会话列表',
    note: '设备字段名是 c_devid（不是 devid）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/streams"`,
    resp: `[{ "c_devid": "mp135", "c_sid": 7, "a_user": "box01", "a_sid": 3, "state": "OPEN" }]`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/clients', minRole: 'operator',
    summary: '在线客户端（A 端）列表',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/clients"`,
  },
  {
    group: 'ops', method: 'DELETE', path: '/api/v1/streams/:a_sid', minRole: 'manager',
    summary: '断开一条会话',
    params: [{ name: 'a_sid', in: 'path', desc: '客户端侧会话 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/streams/12345"`,
  },
  {
    group: 'ops', method: 'GET', path: '/metrics', minRole: 'operator',
    summary: 'relay 自身的 Prometheus 文本指标',
    note: '连接/帧/流/管理面 HTTP/SQLite 耗时；绑定分组的 Token 只看组内设备，全局累计计数不返回',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/metrics"`,
  },

  // ---------------------------------------------------------------- 部署
  {
    group: 'ops', method: 'GET', path: '/api/v1/deploy/hosts', minRole: 'manager',
    summary: '部署目标主机列表（不回显密码/私钥）',
    note: '凭据只在 relay 本机 `<admin.db 同目录>/deploy/`（0600）；`secret_set` 只表示有没有存过',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/hosts"`,
    resp: `{ "items": [{ "id": 1, "name": "车间-01", "host": "10.0.0.21", "port": 22, "username": "root", "auth": "password", "sudo": false, "secret_set": true, "last_status": "ok" }] }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/deploy/hosts', minRole: 'manager',
    summary: '登记一台可 SSH 登录的目标主机',
    note: '`auth` 取 password 或 key；`secret` 是密码或私钥全文，只写不读',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"车间-01","host":"10.0.0.21","port":22,"username":"root","auth":"password","secret":"******"}' \\\n  "$RT/api/v1/deploy/hosts"`,
    resp: `{ "id": 1 }`,
  },
  {
    group: 'ops', method: 'PUT', path: '/api/v1/deploy/hosts/:id', minRole: 'manager',
    summary: '修改主机信息（secret 留空表示不改凭据）',
    params: [{ name: 'id', in: 'path', desc: '主机 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"车间-01","host":"10.0.0.21","port":22,"username":"root","auth":"key","secret":"-----BEGIN OPENSSH PRIVATE KEY-----"}' \\\n  "$RT/api/v1/deploy/hosts/1"`,
  },
  {
    group: 'ops', method: 'DELETE', path: '/api/v1/deploy/hosts/:id', minRole: 'manager',
    summary: '删除主机与凭据（有执行中的作业时 409）',
    params: [{ name: 'id', in: 'path', desc: '主机 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/hosts/1"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/deploy/hosts/:id/probe', minRole: 'manager',
    summary: 'SSH 连通性测试 + 环境探测',
    note: '返回架构、是否 root、是否 systemd、/tmp 剩余空间与已安装版本；失败返回 502 与原因',
    params: [{ name: 'id', in: 'path', desc: '主机 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/hosts/1/probe"`,
    resp: `{ "ok": true, "arch": "x86_64", "uid": 0, "systemd": true, "free_kb": 8123456, "installed": "none", "raw": "..." }`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/deploy/packages', minRole: 'manager',
    summary: 'deploy/ 目录下的安装包（自动识别组件/版本/架构）',
    note: '包名约定 `rtunnel-<component>[-<variant>]-<version>-<arch>.tar.gz`',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/packages"`,
    resp: `{ "items": [{ "name": "rtunnel-agent-web-1.4.151-x86_64.tar.gz", "component": "agent", "version": "1.4.151", "arch": "x86_64", "variant": "web", "size": 4212345 }] }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/deploy/packages/:name', minRole: 'manager',
    summary: '上传安装包（分片，固定落到仓库 deploy/）',
    note: '与 `/api/v1/files` 相同的分片协议：`?append=1&upload=ID&offset=N` 追加，`?finalize=1&upload=ID` 落盘；单包上限见 `file.max_upload_mb`',
    params: [{ name: 'name', in: 'path', desc: '包文件名（.tar.gz）' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" --data-binary @rtunnel-agent-web-1.4.151-x86_64.tar.gz \\\n  "$RT/api/v1/deploy/packages/rtunnel-agent-web-1.4.151-x86_64.tar.gz"`,
    resp: `{ "name": "rtunnel-agent-web-1.4.151-x86_64.tar.gz", "size": 4212345, "sha256": "..." }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/deploy/jobs', minRole: 'manager',
    summary: '创建部署作业（后台执行 ssh/scp/install.sh）',
    note: '同一主机同时只允许一个未结束的作业；agent 必须给 `options.devid` 与 `options.server`',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"host_id":1,"component":"agent","package":"rtunnel-agent-web-1.4.151-x86_64.tar.gz","variant":"web","options":{"devid":"board01","server":"10.0.0.10:18443","allow":"22"}}' \\\n  "$RT/api/v1/deploy/jobs"`,
    resp: `{ "id": 12 }`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/deploy/jobs', minRole: 'manager',
    summary: '部署作业列表（不含日志正文）',
    params: [{ name: 'limit', in: 'query', desc: '条数，默认 30，最大 200' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/jobs?limit=20"`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/deploy/jobs/:id', minRole: 'manager',
    summary: '单个作业详情（含实时日志，轮询这个接口）',
    params: [{ name: 'id', in: 'path', desc: '作业 ID' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/jobs/12"`,
    resp: `{ "id": 12, "component": "agent", "status": "running", "step": "安装", "log": "[...] 执行包内 install.sh" }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/deploy/jobs/:id/cancel', minRole: 'manager',
    summary: '取消部署（终止 ssh 子进程）',
    params: [{ name: 'id', in: 'path', desc: '作业 ID' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/deploy/jobs/12/cancel"`,
  },

  // ---------------------------------------------------------------- 中继列表
  {
    group: 'ops', method: 'GET', path: '/api/v1/relay-dict', minRole: 'manager',
    summary: '中继地址字典（分页，含是否已加入下发列表）',
    params: [
      { name: 'q', in: 'query', desc: '按地址/名称/备注搜索' },
      { name: 'page', in: 'query', desc: '页码，默认 1' },
      { name: 'page_size', in: 'query', desc: '每页条数，默认 50' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-dict?page=1&page_size=20"`,
    resp: `{ "items": [{ "id": 1, "host": "10.0.0.2", "port": 18443, "name": "主 relay", "enabled": true, "in_list": true, "position": 1 }], "total": 1 }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-dict', minRole: 'manager',
    summary: '新增中继地址',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"host":"10.0.0.3","port":18443,"name":"备 relay","description":"机房 B"}' \\\n  "$RT/api/v1/relay-dict"`,
    resp: `{ "id": 2 }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-dict/batch', minRole: 'manager',
    summary: '批量导入中继地址',
    note: '每行 `地址 [名称] [备注...]`，地址支持 host、host:port、[v6]:port；重复地址自动跳过',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"text":"10.0.0.3:18443 备 relay 机房 B\\n10.0.0.4:18443 灾备"}' \\\n  "$RT/api/v1/relay-dict/batch"`,
    resp: `{ "added": 2, "duplicated": 0, "invalid": 0 }`,
  },
  {
    group: 'ops', method: 'PUT', path: '/api/v1/relay-dict/:id', minRole: 'manager',
    summary: '编辑中继地址（被下发列表引用时不能停用）',
    params: [{ name: 'id', in: 'path', desc: '字典 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"host":"10.0.0.3","port":18443,"name":"备 relay","enabled":true}' \\\n  "$RT/api/v1/relay-dict/2"`,
  },
  {
    group: 'ops', method: 'DELETE', path: '/api/v1/relay-dict/:id', minRole: 'manager',
    summary: '删除中继地址（在下发列表中时返回 409）',
    params: [{ name: 'id', in: 'path', desc: '字典 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-dict/2"`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/relay-list', minRole: 'manager',
    summary: '有序下发列表 + 各在线设备的 ACK',
    note: '`rev` 是列表内容哈希；`agents[].ok/rev/persisted` 判断设备是否已对齐',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-list"`,
    resp: `{ "servers": "10.0.0.2:18443,10.0.0.3:18443", "rev": 1448700810, "configured": true, "allow_leave": false, "agents": [{ "devid": "mp135", "ok": true, "rev": 1448700810, "persisted": true }] }`,
  },
  {
    group: 'ops', method: 'PUT', path: '/api/v1/relay-list', minRole: 'manager',
    summary: '保存有序下发列表（只改期望状态，不推送）',
    note: '最多 8 个；顺序即故障切换顺序。主动下发用同步任务或 `POST /api/v1/relay-list/push`',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"endpoint_ids":[1,2],"allow_leave":false}' "$RT/api/v1/relay-list"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-list/push', minRole: 'manager',
    summary: '对全部在线设备（或指定 devids）立即下发当前列表',
    note: '内部创建等同的一次性同步任务，返回 batch',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{}' "$RT/api/v1/relay-list/push"`,
    resp: `{ "batch": "r1795453048766-3fa2da", "rev": 1448700810, "total": 2 }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-sync/tasks', minRole: 'manager',
    summary: '创建中继列表同步任务（一次性或分批灰度）',
    note: '任务快照当前列表；离线设备记 pending，上线自动对齐；失败率超阈值暂停',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"中继切换-1124","devids":["mp135","mp135b"],"mode":"wave","wave_size":1,"wave_interval_s":600,"fail_threshold_pct":20}' \\\n  "$RT/api/v1/relay-sync/tasks"`,
    resp: `{ "batch": "r1795453048766-3fa2da", "rev": 1448700810, "total": 2, "total_waves": 2 }`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/relay-sync/tasks', minRole: 'manager',
    summary: '同步任务列表',
    params: [
      { name: 'status', in: 'query', desc: 'active / done / failed_group 或具体状态' },
      { name: 'page', in: 'query', desc: '页码' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks?status=active"`,
  },
  {
    group: 'ops', method: 'GET', path: '/api/v1/relay-sync/tasks/:batch', minRole: 'manager',
    summary: '同步任务详情（逐设备状态与 ACK）',
    params: [{ name: 'batch', in: 'path', desc: '任务号' }],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks/r1795453048766-3fa2da"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-sync/tasks/:batch/pause', minRole: 'manager',
    summary: '暂停任务（不再释放下一批，已发出的继续等 ACK）',
    params: [{ name: 'batch', in: 'path', desc: '任务号' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks/r1795453048766-3fa2da/pause"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-sync/tasks/:batch/resume', minRole: 'manager',
    summary: '继续任务（立即释放下一批）',
    params: [{ name: 'batch', in: 'path', desc: '任务号' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks/r1795453048766-3fa2da/resume"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-sync/tasks/:batch/cancel', minRole: 'manager',
    summary: '取消任务（未完成/待上线的设备标记取消）',
    params: [{ name: 'batch', in: 'path', desc: '任务号' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks/r1795453048766-3fa2da/cancel"`,
    resp: `{ "ok": true, "cancelled": 1 }`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/relay-sync/tasks/:batch/retry', minRole: 'manager',
    summary: '重试失败/超时的设备（pause/resume/cancel 同一路径）',
    params: [{ name: 'batch', in: 'path', desc: '任务号' }],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/relay-sync/tasks/r1795453048766-3fa2da/retry"`,
    resp: `{ "ok": true, "retried": 1 }`,
  },

  // ---------------------------------------------------------------- 账号/系统
  {
    group: 'sys', method: 'GET', path: '/api/v1/me', minRole: 'viewer',
    summary: '当前身份（Token 归属账号与角色）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/me"`,
    resp: `{ "id": 2, "username": "deploy", "role": "manager", "display_name": "部署账号" }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/me/profile', minRole: 'viewer',
    summary: '个人偏好',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/me/profile"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/me/profile', minRole: 'viewer',
    summary: '修改个人偏好（影响归属账号的网页显示）',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"default_page":"agents"}' "$RT/api/v1/me/profile"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/me/logout-all', minRole: 'viewer',
    summary: '注销归属账号的全部网页会话',
    note: '不会吊销当前 Token',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/me/logout-all"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/users', minRole: 'manager',
    summary: '管理台账号列表',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/users"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/users', minRole: 'manager',
    summary: '新建账号（manager 只能建 operator/viewer）',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"username":"ops1","password":"Passw0rd!23","role":"operator"}' "$RT/api/v1/users"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/users/:id/password', minRole: 'viewer',
    summary: '改密码（改自己需旧密码；manager 可重置下级）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"old_password":"old-pass","new_password":"New-pass-123"}' \\\n  "$RT/api/v1/users/5/password"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/users/:id/status', minRole: 'manager',
    summary: '启用/停用账号（停用会同时使其 Token 失效）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"status":"disabled"}' "$RT/api/v1/users/5/status"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/users/:id/role', minRole: 'manager',
    summary: '调整账号角色（不能操作同级/上级）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"role":"manager"}' "$RT/api/v1/users/5/role"`,
  },
  {
    group: 'sys', method: 'DELETE', path: '/api/v1/users/:id', minRole: 'manager',
    summary: '删除账号',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/users/5"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/devices', minRole: 'manager',
    summary: '设备资源列表（含离线）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/devices"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/settings', minRole: 'viewer',
    summary: '读取系统设置（非 admin 敏感项脱敏）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/settings"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/settings', minRole: 'admin',
    summary: '修改系统设置',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"agent.offline_timeout_sec":90}' "$RT/api/v1/settings"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/alerts/status', minRole: 'admin',
    summary: '告警通道状态',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/alerts/status"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/alerts/test', minRole: 'admin',
    summary: '发送测试告警',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"text":"rtunnel test"}' "$RT/api/v1/alerts/test"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/audit', minRole: 'manager',
    summary: '审计日志（固定最近 200 条）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/audit"`,
    resp: `[{ "at": "2026-11-17 10:12:03", "role": "deploy", "action": "files.edit", "detail": "fw.bin size=1024" }]`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/usage', minRole: 'manager',
    summary: '用量报表（日聚合）：流量、会话、设备在线时长',
    note: '仓库是全局资源，需要 usage.read。metric=bytes|sessions|session_seconds|online_seconds；'
      + 'subject_type=token|device；granularity=day|total。日按 relay 本地时区，日期为 YYYY-MM-DD。',
    params: [
      { name: 'from', in: 'query', desc: '起始日 YYYY-MM-DD（含）' },
      { name: 'to', in: 'query', desc: '结束日 YYYY-MM-DD（含）' },
      { name: 'metric', in: 'query', desc: 'bytes / sessions / session_seconds / online_seconds' },
      { name: 'subject_type', in: 'query', desc: 'token / device' },
      { name: 'subject_id', in: 'query', desc: '令牌 id 或 devid' },
      { name: 'granularity', in: 'query', desc: 'day（默认）或 total' },
      { name: 'tenant_id', in: 'query', desc: '只统计该租户（控制台账号可用；接入令牌固定为本租户）' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" \\\n  "$RT/api/v1/usage?from=2026-11-01&to=2026-11-30&metric=bytes&granularity=day"`,
    resp: `{ "rows": [{ "day": "2026-11-25", "tenant_id": 1, "subject_type": "token", "subject_id": "3", "metric": "bytes", "qty": 1048576, "events": 288 }], "granularity": "day" }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/tenants', minRole: 'manager',
    summary: '租户列表（账号数、设备数、在线数、套餐与当月用量）',
    note: '租户是计费与设备归属边界。设备与接入账号各归属一个租户，接入令牌只能访问本租户设备。'
      + 'used_bytes_month 来自用量日聚合，用于展示；数据面强制用的是 relay 内存里的实时值。',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/tenants"`,
    resp: `{ "items": [{ "id": 1, "name": "默认租户", "note": "", "status": "active", "accounts": 2, "devices": 8, "online": 5, "max_devices": 0, "max_sessions": 0, "quota_bytes": 0, "used_bytes_month": 0 }] }`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/tenants', minRole: 'manager',
    summary: '新建租户',
    note: '需要 tenant.write。名称唯一，1-64 字节。',
    body: `{ "name": "某客户", "note": "可选" }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"某客户"}' "$RT/api/v1/tenants"`,
    resp: `{ "ok": true, "id": 2 }`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/tenants/:id', minRole: 'manager',
    summary: '修改租户名称/备注/启停/套餐',
    note: '需要 tenant.write。停用租户等价于停用其下全部接入账号（在线连接会被断开）；默认租户不可停用。'
      + 'max_devices/max_sessions/quota_bytes/quota_gb 省略表示保持不变，0 表示不限；'
      + '设备数按已登记设备计（默认租户不受限），并发与月流量在数据面强制。',
    body: `{ "name": "某客户", "note": "", "status": "active|disabled", "max_devices": 10, "max_sessions": 20, "quota_gb": 100 }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"某客户","max_devices":10,"quota_gb":100}' "$RT/api/v1/tenants/2"`,
    resp: `{ "ok": true }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/billing/prices', minRole: 'manager',
    summary: '价目表（每个指标的计价单位与单价）',
    note: '金额单位是 1e-6 元。默认单价为 0，即只试算不收费。需要 billing.read。',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/billing/prices"`,
    resp: `{ "items": [{ "metric": "bytes", "unit_size": 1073741824, "price_micro": 500000, "currency": "CNY" }] }`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/billing/prices/:metric', minRole: 'manager',
    summary: '设置某指标的计价单位与单价',
    note: '需要 billing.write（默认只有超级管理员）。metric 取 bytes|sessions|session_seconds|online_seconds；'
      + 'price_yuan 以元为单位，避免前端换算丢精度。',
    body: `{ "unit_size": 1073741824, "price_yuan": 0.5, "currency": "CNY" }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"unit_size":1073741824,"price_yuan":0.5}' "$RT/api/v1/billing/prices/bytes"`,
    resp: `{ "ok": true }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/billing/summary', minRole: 'manager',
    summary: '按租户试算费用（用量 × 单价）',
    note: '需要 billing.read。接入令牌只能算本租户；控制台账号可按 tenant_id 过滤。'
      + '金额向下取整到 0.000001 元，仅用于核算，不生成账单。',
    params: [
      { name: 'from', in: 'query', desc: '起始日 YYYY-MM-DD（含）' },
      { name: 'to', in: 'query', desc: '结束日 YYYY-MM-DD（含）' },
      { name: 'tenant_id', in: 'query', desc: '只算该租户' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/billing/summary?from=2026-11-01&to=2026-11-30"`,
    resp: `{ "tenants": [{ "tenant_id": 1, "tenant_name": "默认租户", "items": [{ "metric": "bytes", "qty": 1073741824, "amount_micro": 500000 }], "total_micro": 500000 }], "total_micro": 500000, "currency": "CNY" }`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/billing/statements', minRole: 'manager',
    summary: '生成或重算账期结算单',
    note: '需要 billing.write。只处理草稿：已有草稿按最新用量重算，已出账的记录跳过。'
      + 'tenant_id 省略或 0 表示所有租户。',
    body: `{ "period": "2026-11", "tenant_id": 0 }`,
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"period":"2026-11"}' "$RT/api/v1/billing/statements"`,
    resp: `{ "period": "2026-11", "from": "2026-11-01", "to": "2026-11-31", "created": [{ "id": 1, "tenant_id": 1, "total_micro": 500000 }], "updated": [], "skipped": [] }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/billing/statements', minRole: 'manager',
    summary: '结算单列表（含明细快照）',
    note: '接入令牌只能看本租户。',
    params: [
      { name: 'period', in: 'query', desc: '账期 YYYY-MM' },
      { name: 'status', in: 'query', desc: 'draft|issued|paid|void' },
      { name: 'tenant_id', in: 'query', desc: '只列该租户（控制台账号可用）' },
    ],
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/billing/statements?period=2026-11"`,
    resp: `{ "items": [{ "id": 1, "tenant_id": 1, "tenant_name": "默认租户", "period": "2026-11", "total_micro": 500000, "status": "draft", "lines": [{ "metric": "bytes", "qty": 1073741824, "unit_size": 1073741824, "price_micro": 500000, "amount_micro": 500000 }], "note": "", "created_ms": 0, "updated_ms": 0, "issued_ms": 0, "paid_ms": 0 }] }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/billing/statements/:id', minRole: 'manager',
    summary: '结算单详情',
    note: '接入令牌读取别家结算单返回 403。',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/billing/statements/1"`,
  },
  {
    group: 'sys', method: 'PUT', path: '/api/v1/billing/statements/:id', minRole: 'manager',
    summary: '结算单状态流转',
    note: '需要 billing.write。draft→issued→paid；draft/issued 可→void，paid/void 为终态。'
      + 'issued 与 paid 会记录时间戳，note 可选。',
    body: `{ "status": "issued", "note": "2026-11 账期" }`,
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"status":"issued"}' "$RT/api/v1/billing/statements/1"`,
    resp: `{ "id": 1, "status": "issued", "issued_ms": 1764000000000, ... }`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/billing/statements/:id/csv', minRole: 'manager',
    summary: '导出结算单 CSV（Excel 可直接打开）',
    note: '带 UTF-8 BOM；一行一个指标明细，接入令牌只能导出本租户。',
    curl: `curl -sS -H "Authorization: Bearer $ATK" -o statement-1.csv "$RT/api/v1/billing/statements/1/csv"`,
  },
  {
    group: 'sys', method: 'GET', path: '/api/v1/certs', minRole: 'manager',
    summary: '证书台账',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/certs"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/certs/issue', minRole: 'admin',
    summary: '签发证书（CA 在本机）',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"san":"mp135","role":"device","days":365}' "$RT/api/v1/certs/issue"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/certs/revoke', minRole: 'admin',
    summary: '吊销证书',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"san":"mp135"}' "$RT/api/v1/certs/revoke"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/certs/sign', minRole: 'admin',
    summary: '签发 CSR（私钥不出设备）',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"csr":"-----BEGIN CERTIFICATE REQUEST-----\\n...","days":365}' \\\n  "$RT/api/v1/certs/sign"`,
  },
  {
    group: 'sys', method: 'POST', path: '/api/v1/shutdown', minRole: 'admin',
    summary: '优雅关停 relay',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/shutdown"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/dicts', minRole: 'operator',
    summary: '字典分页：资源 / 端口 / 串口',
    params: [
      { name: 'kind', in: 'query', desc: 'resources | devices | ports | ttys' },
      { name: 'page', in: 'query', desc: '页码，从 1 开始' },
      { name: 'page_size', in: 'query', desc: '每页条数，默认 10，最大 200' },
      { name: 'q', in: 'query', desc: '按键/值/名称搜索' },
    ],
    note: '字典页使用；增删改查走 /permission/dict/ports|ttys',
    curl: `curl -sS -H "Authorization: Bearer $ATK" \\\n  "$RT/api/v1/permission/dicts?kind=ports&page=1&page_size=10"`,
    resp: `{ "kind": "ports", "page": 1, "page_size": 10, "total": 23, "items": [{ "id": 1, "value": "22", "name": "ssh" }] }`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/resources', minRole: 'operator',
    summary: '资源字典（管理台全部可授权动作）',
    note: '资源键在代码中登记，接口只能改显示名/说明/启用状态',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/resources"`,
    resp: `[{ "key": "device.ota.write", "category": "设备", "name": "OTA 升级开关", "enabled": true }]`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/resources/:key', minRole: 'manager',
    summary: '修改资源显示名 / 说明 / 启用状态',
    params: [
      { name: 'key', in: 'path', desc: '资源键' },
      { name: 'name', in: 'body', desc: '显示名' },
      { name: 'description', in: 'body', desc: '说明' },
      { name: 'enabled', in: 'body', desc: '是否启用' },
    ],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"OTA 升级开关","enabled":true}' \\\n  "$RT/api/v1/permission/resources/device.ota.write"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/groups', minRole: 'operator',
    summary: '权限组列表（含资源与成员数）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/groups"`,
    resp: `[{ "id": 2, "name": "管理员", "builtin_key": "manager", "super": false, "resources": ["device.view"] }]`,
  },
  {
    group: 'perm', method: 'POST', path: '/api/v1/permission/groups', minRole: 'manager',
    summary: '新建权限组（功能资源 + 设备 + 成员一次保存）',
    params: [
      { name: 'name', in: 'body', desc: '组名，唯一' },
      { name: 'description', in: 'body', desc: '说明' },
      { name: 'resources', in: 'body', desc: '功能资源键数组' },
      { name: 'devices', in: 'body', desc: '设备 devid 数组；all_devices=true 时忽略' },
      { name: 'all_devices', in: 'body', desc: 'true = 全部设备（含以后新增）' },
      { name: 'ports', in: 'body', desc: '组级 TCP 端口范围，如 22,80-90 或 *' },
      { name: 'members', in: 'body', desc: '[{type:user|atk|client|rtk, id}]' },
    ],
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"产线运维","resources":["device.view","file.read"],"devices":["mp135"],"ports":"22","members":[{"type":"user","id":"2"}]}' \\\n  "$RT/api/v1/permission/groups"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/groups/:id', minRole: 'operator',
    summary: '权限组详情：资源、设备、成员',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/groups/5"`,
    resp: `{ "id": 5, "name": "产线运维", "all_devices": false, "ports": "22", "resources": ["device.view"], "devices": ["mp135"], "members": [{ "type": "user", "id": "2" }] }`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/subjects', minRole: 'operator',
    summary: '授权对象：账号 / atk / 接入账号 / rtk 及其有效权限',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/subjects"`,
    resp: `[{ "type": "user", "id": "2", "name": "alice", "super": false, "resource_count": 3, "device_count": 1, "groups": [{ "id": 5, "name": "产线运维" }] }]`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/subjects/:type/:id/groups', minRole: 'operator',
    summary: '授权对象所属权限组（type=user|atk|client|rtk）',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/subjects/user/2/groups"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/subjects/:type/:id/groups', minRole: 'manager',
    summary: '设置授权对象的权限组（整组覆盖）',
    params: [{ name: 'ids', in: 'body', desc: '权限组 id 数组' }],
    note: 'atk/rtk 的权限不会超过归属账号；清空后不再按角色回落',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ids":[5]}' "$RT/api/v1/permission/subjects/user/2/groups"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/devices/:devid', minRole: 'manager',
    summary: '设备资源：改名 / 临时停用',
    params: [
      { name: 'devid', in: 'path', desc: '设备 ID' },
      { name: 'name', in: 'body', desc: '显示名' },
      { name: 'enabled', in: 'body', desc: 'false = 所有权限组暂时看不到这台设备' },
    ],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"产线盒子","enabled":true}' "$RT/api/v1/permission/devices/mp135"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/groups/:id', minRole: 'manager',
    summary: '修改权限组与资源',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"仓库只读","resources":["file.read","file.write"]}' \\\n  "$RT/api/v1/permission/groups/5"`,
  },
  {
    group: 'perm', method: 'DELETE', path: '/api/v1/permission/groups/:id', minRole: 'manager',
    summary: '删除权限组（预置组不可删）',
    curl: `curl -sS -X DELETE -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/groups/5"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/permission/groups/:id/members', minRole: 'operator',
    summary: '权限组成员与候选账号',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/permission/groups/2/members"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/permission/groups/:id/members', minRole: 'manager',
    summary: '设置权限组成员（整组覆盖，支持四类主体）',
    params: [{ name: 'members', in: 'body', desc: '[{type,id}]，type=user|atk|client|rtk' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"members":[{"type":"user","id":"2"}]}' "$RT/api/v1/permission/groups/2/members"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/users/:id/perm-groups', minRole: 'manager',
    summary: '账号所属权限组',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/users/2/perm-groups"`,
  },
  {
    group: 'perm', method: 'GET', path: '/api/v1/client-accounts/:id/perm-groups', minRole: 'manager',
    summary: '接入账号所属资源权限组',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/client-accounts/1/perm-groups"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/client-accounts/:id/perm-groups', minRole: 'manager',
    summary: '设置接入账号的资源权限组（rtk Token 继承）',
    params: [{ name: 'ids', in: 'body', desc: '权限组 id 数组' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ids":[2]}' "$RT/api/v1/client-accounts/1/perm-groups"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/users/:id/perm-groups', minRole: 'manager',
    summary: '设置账号的权限组（整组覆盖）',
    params: [{ name: 'ids', in: 'body', desc: '权限组 id 数组' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"ids":[2,5]}' "$RT/api/v1/users/2/perm-groups"`,
  },
]

// API Token 无法使用的接口（仅账号会话可用），用于文档中的「不可用能力」说明
export const ATK_UNAVAILABLE = [
  'GET/POST/PUT/DELETE /api/v1/admin-tokens* —— API Token 不能管理 Token（403）',
  'POST /api/v1/login、/api/v1/logout、/api/v1/setup* —— 仅网页会话/初始化',
]

export const ERRORS = [
  ['401', '未认证 / Token 停用、吊销、过期 / 归属账号停用'],
  ['403', '缺少对应资源权限，或设备不在授权范围内'],
  ['404', '设备离线、文件不存在、任务不存在'],
  ['409', '分片 offset 不匹配、目标文件冲突'],
  ['413', '超过 file.max_upload_mb / file.max_edit_mb'],
  ['504', '设备未在超时时间内应答'],
]

