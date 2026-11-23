// 管理台 HTTP 接口文档（atk Token / 账号会话通用）。
// 唯一数据源：/docs 页面与「API Token · 用法」抽屉都读这里。
// 新增路由时必须同步本文件，scripts/check-api-docs.mjs 会对表 admin_routes.cpp。

const ROLE_WEIGHT = { viewer: 1, operator: 2, manager: 3, admin: 4 }

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
    ],
    note: 'path 必须落在 mount 内；大目录返回 gzip（按 Accept-Encoding），最多等 30s',
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
    group: 'perm', method: 'POST', path: '/api/v1/client-accounts/:id/tokens', minRole: 'manager',
    summary: '为接入账号创建 Token（配额/限速在这里）',
    params: [{ name: 'id', in: 'path', desc: '账号 ID' }],
    note: 'rate_per_min/burst/max_sessions/quota_gb/quota_period/quota_action 可选',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"name":"box02-main","expires_in_days":180,"rate_per_min":600,"burst":100,\n       "max_sessions":16,"quota_gb":50,"quota_period":"month","quota_action":"block"}' \\\n  "$RT/api/v1/client-accounts/1/tokens"`,
  },
  {
    group: 'perm', method: 'PUT', path: '/api/v1/client-tokens/:id', minRole: 'manager',
    summary: '修改接入 Token（未传字段保持原值）',
    params: [{ name: 'id', in: 'path', desc: 'Token ID' }],
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"rate_per_min":1200,"status":"active"}' "$RT/api/v1/client-tokens/7"`,
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
  {
    group: 'ops', method: 'GET', path: '/api/v1/acl/file', minRole: 'operator',
    summary: '读取 ACL 文件内容与解析结果',
    curl: `curl -sS -H "Authorization: Bearer $ATK" "$RT/api/v1/acl/file"`,
  },
  {
    group: 'ops', method: 'PUT', path: '/api/v1/acl/file', minRole: 'manager',
    summary: '写入 ACL 文件并重载（失败保留旧规则）',
    curl: `curl -sS -X PUT -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \\\n  -d '{"content":"box01=dev-*:80,443\\n"}' "$RT/api/v1/acl/file"`,
  },
  {
    group: 'ops', method: 'POST', path: '/api/v1/acl/reload', minRole: 'manager',
    summary: '热加载 ACL 文件',
    curl: `curl -sS -X POST -H "Authorization: Bearer $ATK" "$RT/api/v1/acl/reload"`,
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
  ['403', '角色不足，或设备不在 Token 绑定的分组内'],
  ['404', '设备离线、文件不存在、任务不存在'],
  ['409', '分片 offset 不匹配、目标文件冲突'],
  ['413', '超过 file.max_upload_mb / file.max_edit_mb'],
  ['504', '设备未在超时时间内应答'],
]

function roleWeight(role) {
  return ROLE_WEIGHT[role] || 0
}

// 按角色过滤：atk 与账号会话一致；admin-tokens 永远不出现
export function docsForRole(role) {
  const w = roleWeight(role)
  if (!w) return []
  return DOCS.filter((d) => roleWeight(d.minRole) <= w)
}
