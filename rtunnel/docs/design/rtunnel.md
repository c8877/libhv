# RTNP 协议（v1）

反向通道协议，用于 client ↔ relay ↔ agent 之间的控制、数据与设备管理。
代码入口：`rtnp/frame.hpp`（帧类型、错误码）、`rtnp/codec.hpp`（编解码）、
`common/tunnel_link.cpp`（握手、心跳、重连）。

术语：

| 角色 | 说明 |
|---|---|
| C / agent | 设备侧，主动连 relay，注册 devid |
| B / relay | 中继，转发流、缓存快照、提供管理面 |
| A / client | 访问侧，按 `用户@devid:端口` 打开会话；管理台内部链路也算 A |

## 1. 帧格式

固定 16 字节大端头 + payload：

```
magic[4]="RTNP" | version(1) | type(1) | flags(2) | stream_id(4) | length(2) | reserved(2)
```

- `version`：**帧格式版本**，两端必须一致，不一致就断开（当前 1）。只有帧头布局变化才 +1。
- `type`：见第 3 节。
- `flags`：仅 `OPEN` 使用，见第 6 节；其它帧填 0。
- `stream_id`：多路复用号，由请求方分配；响应帧回显。
- `length`：payload 长度，上限 `MAX_PAYLOAD = 32768`。超过要拆成多个同类型帧，接收端按序拼接
  （WebSocket 代理就是这样处理大消息的）。
- `reserved`：必须为 0。

字符串统一 `u16 len + bytes`，不带 NUL。整数大端。

## 2. 握手与认证

```
C → B   HELLO      u8 role | u8 proto | str identity | u32 0
B → C   HELLO_ACK  u16 max_payload | u8 auth_flags | u32 heartbeat_ms | u32 0
                   [u8 relay_proto | u8 relay_proto_min]        # 可选尾部
C → B   AUTH       str identity | u8 auth_type | str token
B → C   AUTH_OK    str username | u32 heartbeat_ms | [u8 flags | u32 quota | ...]
        AUTH_FAIL  str reason 然后断开
```

- `role`：1=agent，2=client。
- `proto`：**语义版本**，见第 9 节。
- `identity`：agent 填 devid，client 填用户名。`HELLO` 里额外带一个 `u32`（保留，填 0）。
- `auth_type`：0=无凭据（agent），1=静态 token，2=管理台签发的账号 token。
- `HELLO_ACK` 尾部两个字节是 relay 的语义版本与最低支持版本；旧 relay 不发送，
  旧 client 读到多余字节直接忽略。client 只记录，具体能力由业务帧自行判断。
- 握手超时 5 s，认证超时 10 s。

心跳：client/agent 每 15 s 发 `PING`，relay 回 `PONG`。连续 3 次未收到 `PONG`
主动重连。`PING`/`PONG` 无 payload。

## 3. 帧类型

连接级（`sid` 多数为请求号）：

| 值 | 名称 | 方向 | payload |
|---|---|---|---|
| 0x01 | HELLO | C→B | 见第 2 节 |
| 0x02 | HELLO_ACK | B→C | 见第 2 节 |
| 0x03 | AUTH | C→B | 见第 2 节 |
| 0x04 | AUTH_OK | B→C | 见第 2 节 |
| 0x05 | AUTH_FAIL | B→C | str 原因 |
| 0x06 | PING | 双向 | 空 |
| 0x07 | PONG | 双向 | 空 |
| 0x08 | DEVICE_QUERY | B→C | str devid |
| 0x09 | DEVICE_STATUS | C→B | u8 online + u32 活跃流数 |
| 0x0F | ERROR | B→C | u32 code + str message |

会话/流：

| 值 | 名称 | 方向 | payload |
|---|---|---|---|
| 0x10 | OPEN | B→C | 端口或 flags 决定，见第 6 节 |
| 0x11 | OPEN_OK | C→B | str label + u16 保留 |
| 0x12 | OPEN_FAIL | B→C | u32 code + str message |
| 0x13 | DATA | 双向 | 原始字节（shell/tcp）；fs/svc 首字节 0=JSON 1=二进制 |
| 0x14 | WINDOW | 双向 | u32 增量字节数 |
| 0x15 | CLOSE | 双向 | 空 |
| 0x16 | RESET | 双向 | 空（异常终止） |
| 0x17 | RESIZE | B→C | u16 cols + u16 rows |

设备管理（B→C 请求、C→B 响应）：

| 值 | 名称 | payload |
|---|---|---|
| 0x18 | SPAWN_AGENT | str devid |
| 0x19 | SYSINFO_REQ | 空 |
| 0x1A | SYSINFO | str json，见 7.1 |
| 0x1B | CONFIG_SET | str json `{ports?,shell?,file?,file_rw?,svc?,svc_rw?,ota?,reboot?,poweroff?,diag?,metrics?}` |
| 0x1C | CONFIG_ACK | str json `{ok,error?,restart_required?}` |
| 0x1D | FILE_BEGIN | str name + u64 size + str sha256 |
| 0x1E | FILE_DATA | 原始字节 |
| 0x1F | FILE_END | str sha256 |
| 0x20 | FILE_ACK | str json `{ok,received,error?}` |
| 0x21 | FILE_CHECK_REQ | str dir + str name |
| 0x22 | FILE_CHECK_ACK | str json `{exists,size,sha256,error?}` |
| 0x29 | TTY_LIST_REQ | 空 |
| 0x2A | TTY_LIST_ACK | str json `[{path,by_id,configured,enabled,baud,data_bits,parity,stop_bits,rx,tx}]` |
| 0x2B | TTY_CFG_SET | str json `[{path,enabled,baud,data_bits,parity,stop_bits}]` |
| 0x2C | METRICS_CFG | str json `[{series,enabled,interval_sec,points}]` |
| 0x2D | METRICS_STATUS | str json `{applied,error?}` |
| 0x2E | METRICS_QUERY | str json `{series,since_ts}` |
| 0x2F | METRICS_DATA | str json `{series,points:[{ts,v,...}],error?}` |
| 0x30 | OTA_APPLY | str json `{path,version,size,sha256}` |
| 0x31 | OTA_APPLY_ACK | str json `{ok,phase,error?}`，phase=verified/installed |
| 0x32 | OTA_CANCEL | 空 |
| 0x33 | NET_REQ | str json `{scope,proc,page,page_size,q,listen,utype,ipc_kind,sort,dir,refresh}` |
| 0x34 | NET_INFO | str json，见 7.3 |
| 0x35 | PROC_REQ | str json `{op,sort,q,kernel,page,page_size,refresh,pid,signal}` |
| 0x36 | PROC_INFO | str json，见 7.2 |
| 0x37 | WARM_REQ | str json `{ttl_ms}` |
| 0x38 | POWER_REQ | str json `{action,delay_ms,password}` |
| 0x39 | POWER_ACK | str json `{ok,action,delay_ms,error?}` |
| 0x3A | DIAG_REQ | str json `{max_mb}` |
| 0x3B | DIAG_INFO | str json `{ok,name,size,sha256,error?}` |
| 0x3C | DIAG_DATA | 原始字节，按序拼接 |
| 0x3D | DIAG_END | str json `{ok,size,sha256,error?}` |
| 0x3E | STORAGE_REQ | str json `{mount,path,depth}` |
| 0x3F | STORAGE_INFO | str json，见 7.4 |
| 0x40 | STORAGE_DATA | 首字节 1=gzip 0=明文，其余按序拼接 |
| 0x41 | STORAGE_END | str json `{ok,size,error?}` |

未识别的类型：relay 回 `ERROR(code=PROTOCOL_ERROR)` 并保留连接；agent 直接忽略。
新增帧必须保证旧端“忽略也不出错”，否则要提升 `PROTO_MIN_VERSION`。

## 4. 错误码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | OK | |
| 1 | UNAUTHORIZED | 未认证 / 凭据无效 |
| 2 | DEVICE_OFFLINE | 设备不在线 |
| 3 | TARGET_REFUSED | 设备侧拒绝连接 |
| 4 | TIMEOUT | 超时 |
| 5 | PROTOCOL_ERROR | 帧或载荷不合法 |
| 6 | LIMIT_EXCEEDED | 超过并发/大小限制 |
| 7 | ACL_DENIED | ACL/权限组拒绝 |
| 8 | DEVICE_REPLACED | 同 devid 新连接顶替旧连接 |
| 9 | TARGET_NOT_ALLOWED | 端口/串口不在白名单 |
| 10 | CLOSE_TIMEOUT | 关闭超时 |
| 11 | CANCELLED | 请求被取消 |
| 12 | TOKEN_EXPIRED | 令牌过期 |
| 13 | TOKEN_DISABLED | 令牌停用 |
| 14 | QUOTA_EXCEEDED | 流量配额用尽 |
| 15 | RATE_LIMITED | 触发频率限制 |
| 16 | SESSION_LIMIT | 并发会话超限 |

## 5. 流控

- 每条流初始信用 `DEFAULT_CREDIT = 128 KiB`，接收方消费后回 `WINDOW`（u32 增量）。
- 低于 `LOW_WATER = 64 KiB` 时发送方暂停读源。
- 连接级在途上限 `CONN_INFLIGHT = 512 KiB`。
- 空闲流由 relay 定期回收（`stream_idle_timeout_ms`），两端都收 `RESET(TIMEOUT)`。

文件与 FS 流方向相反：A→C 的上传用 `FILE_DATA`，C→A 的下载用 `DATA`；
fs/svc 子协议在自己的 `DATA` 里再带一字节 JSON/二进制标记，详见 `agent/fs_session.cpp`、
`agent/svc_service.cpp`。

## 6. OPEN 的 flags

| 位 | 名称 | payload |
|---|---|---|
| 0x0001 | OPEN_SHELL | u16 端口（忽略），设备侧 forkpty |
| 0x0002 | OPEN_TTY | str devid + str tty_devid + str path + termios（baud/data/parity/stop） |
| 0x0004 | OPEN_TTY_MONITOR | 只读监听已打开串口 |
| 0x0008 | OPEN_TTY_TAKEOVER | 强制接管串口，断开现有会话 |
| 0x0010 | OPEN_FS | u8 want_rw + u8 trusted |
| 0x0020 | OPEN_SVC | u8 trusted（可写由 agent 的 `--svc-rw` 决定） |

不带标志时 payload 是 `u16 端口`，转发到设备本地 TCP。
`trusted` 由 relay 填写，agent 据此跳过本地 file/svc token 校验；设备本地 Web 入口填 0。

## 7. 载荷契约（JSON 帧）

通用约定：

- JSON 对象；未列出的字段接收方必须忽略，不要报错。
- 数字统一为 int64/double；时间戳是毫秒（`_ms`）或秒（`sec`/`ts`，见各字段）。
- 字符串缺省为空串，未知数值用 `null`（例如进程已退出时的 `pid`）。
- 分页字段统一 `page`（从 1 开始）、`page_size`、`pages`、`total`、`matched`、`truncated`。

### 7.1 SYSINFO / SYSINFO_REQ

`SYSINFO_REQ` 空载荷；`SYSINFO` 是 `str json`，顶层：

| 字段 | 说明 |
|---|---|
| `ok` | 采集是否成功 |
| `devid` / `version` / `variant` | 设备 id、agent 版本、lite/web 变体 |
| `machine` / `kernel` / `board` | 架构、内核、板型 |
| `cpu_model` / `cpu_count` / `cpu_cores` / `cpu_usage` / `loadavg` | CPU 与负载 |
| `mem_total_kb` / `mem_free_kb` / `mem_available_kb` / `swap_*_kb` | 内存 |
| `uptime_sec` / `agent_uptime_sec` / `ts` | 系统与 agent 运行时长、采集时刻 |
| `mounts[]` | `{mount,fstype,total_kb,used_kb,avail_kb}` |
| `nics[]` | `{name,ip,netmask,mac,up,carrier,operstate,mtu,speed_mbps,rx_bytes,tx_bytes,rx_errors,tx_errors,rx_dropped,tx_dropped}` |
| `serial[]` | `{path,by_id,configured,enabled,baud,data_bits,parity,stop_bits,rx,tx}` |
| `temps[]` | `{zone,type,temp_c}` |
| `disk_health[]` | eMMC/SD 寿命与读写量 |
| `streams[]` | 当前 relay 会话（relay 补填） |
| `caps` | 能力块，见下 |
| `mtls` | 证书信息 `{cert_subject,cert_issuer,cert_serial,cert_valid,...}` |
| `args` / `web` / `relay` / `relay_count` / `relay_index` | 启动参数、本地页、relay 选择 |

`caps` 键：`ports[]`、`allow_tty[]`、`shell`、`ota`、`ota_pkg`、`file`、`fs`、`fs_rw`、
`file_dir`、`fs_root`、`fs_max_mb`、`fs_token`、`svc`、`svc_rw`、`svc_token`、
`net`、`proc`、`metrics`、`sysinfo`、`mtls`、`diag`、`reboot`、`poweroff`、
`local_web`、`warm`、`arch`、`version`、`variant`、`config_file`、`config_loaded`。

`mode=inc` 时只回变化部分，字段可能缺失；`mode=full` 才是完整快照。

### 7.2 PROC_REQ / PROC_INFO

请求 `{op,sort,q,kernel,page,page_size,refresh,pid,signal}`：

- `op`：`list`（默认）/`detail`/`signal`。
- `sort`：`pid|user|comm|cpu|mem|threads|start`；`q` 匹配 pid/命令/用户；`kernel=1` 包含内核线程。
- `signal`：`TERM|KILL|HUP|INT`（`op=signal`，`pid` 必填）。

响应：

| 字段 | 说明 |
|---|---|
| `entries[]` | `{pid,ppid,uid,user,comm,cmd,state,state_text,cpu_pct,rss_kb,threads,start_ms,kernel}` |
| `total` / `matched` / `page` / `page_size` / `pages` / `truncated` | 分页 |
| `sort` / `cached` / `cache_age_ms` / `sample_ms` / `ts` | 排序键与缓存信息 |
| `summary[]` | 概览卡片用的小计（`{k,v}`） |

`cpu_pct` 是**单核百分比**（可 >100，与 top 一致）；监控里的进程曲线是整机占比，两者口径不同。

### 7.3 NET_REQ / NET_INFO

请求 `{scope,proc,page,page_size,q,listen,utype,ipc_kind,sort,dir,refresh}`：

- `scope`：`tcp|udp|unix|ipc|nic`。
- `ipc_kind`：`shm|msg|sem|posix`。
- `utype`：unix socket 类型过滤。
- `sort`/`dir`：服务端排序键与方向（1 升序，-1 降序，0 默认）。
  socket 键：`local|local_addr|local_port|remote|remote_addr|remote_port|state|pid|comm|inode`；
  unix：`path|type|state|pid|comm|inode`；
  ipc：`key|id|perms|owner|size|nattch|pid|comm|used|msgs|lspid|lrpid|nsems|name|mtime_ms`。

响应：`{ok,scope,page,page_size,truncated,ts,proc,summary[],error?}` 加对应段：

- `tcp` / `udp`：`{entries[],total,matched,page,page_size,pages,truncated,listening,connected}`
  entry `{local_addr,local_port,remote_addr,remote_port,state,pid,comm,inode,uid}`
- `unix`：`{entries[],...}`，entry `{path,abstract,type,state,inode,pid,comm}`
- `ipc`：`{kind,entries[],counts[],limits[],source,...}`，entry 字段随 kind 变化
  （shm：`size,nattch,ctime`；msg：`used,msgs,lspid,lrpid`；sem：`nsems`；posix：`name,mtime_ms`）

### 7.4 STORAGE_REQ / STORAGE_INFO / STORAGE_DATA / STORAGE_END

请求 `{mount,path,depth}`。响应 `{ok,mount,path,total_kb,used_kb,free_kb,reserved_kb,bytes,files,dirs,entries,scan_ms,truncated,truncated_reason,skipped,nodes[]}`，
`nodes[]` 形如 `{p,t,b,m,n,d,c}`（路径、类型、字节、mtime、子项数、深度、是否截断）。
大目录用 `STORAGE_DATA`（gzip）流式回传，`STORAGE_END` 收尾并给 sha/大小。

### 7.5 文件：FILE_* / FILE_CHECK_*

`FILE_BEGIN{name,size,sha256}` → 若干 `FILE_DATA` → `FILE_END{sha256}` → `FILE_ACK{ok,received,error?}`。
`FILE_CHECK_*` 用于断点续传前的存在性与哈希检查。
文件管理子协议（list/stat/mkdir/upload/download/archive、分片与续传）在
`agent/fs_session.cpp`，走 `OPEN_FS` 流内的 JSON/二进制 `DATA`。

### 7.6 串口

`TTY_LIST_ACK` 回数组，字段见第 3 节。`TTY_CFG_SET` 是同样的数组子集；
配置结果通过 `CONFIG_ACK` 或后续 `TTY_LIST_ACK` 体现。

### 7.7 采样（METRICS_*）

- 配置：`METRICS_CFG` + `[{series,enabled,interval_sec,points}]`，整体替换；
  返回 `METRICS_STATUS{applied,error?}`。
- `series` 命名：`cpu`、`mem`、`load`、`temp`、`net:<iface>`、`proc:<pid>:cpu`、`proc:<pid>:mem`。
- 只有配置里 `enabled=true` 的指标才采样；停用会清掉该指标的环形缓冲与增量状态。
- 查询：`METRICS_QUERY{series,since_ts}` → `METRICS_DATA{series,points[],error?}`；
  `points[]` 是 `{ts,v}`，进程点额外带 `name`、`rss_kb`、`gone`。

### 7.8 OTA

`OTA_APPLY{path,version,size,sha256}`：文件已由分发通道落到设备 `file_dir`，
agent 校验后安装并重启；`OTA_APPLY_ACK` 分 `verified` / `installed` 两个阶段，
`OTA_CANCEL` 只能在安装开始前生效。

### 7.9 电源与诊断

- `POWER_REQ{action:"reboot"|"poweroff",delay_ms,password}` → `POWER_ACK{ok,action,delay_ms,error?}`；
  `password` 是设备 root 密码，管理台不落库。
- `DIAG_REQ{max_mb}` → `DIAG_INFO` → 若干 `DIAG_DATA` → `DIAG_END`。诊断包是 tar.gz，
  内含 `sysinfo.json`、日志、配置（脱敏）。

## 8. 分页与排序

- 分页在**服务端**：进程、网络/IPC、任务、OTA 任务、文件列表。前端只拿当前页。
- 排序参数统一 `sort` + `dir`（1 升序，-1 降序，0 用默认）。默认方向：数值列降序、
  文本列升序；`sort` 为空时用各接口的默认顺序（进程按 CPU、socket 按监听优先+端口）。
- 非法 `sort` 键必须回退到默认顺序，不能报错。
- relay 的设备代理会把 `sort/dir` 纳入缓存键，避免不同排序命中同一份缓存。

## 9. 兼容与演进

两个版本号，职责不同：

| 版本 | 位置 | 规则 |
|---|---|---|
| 帧格式 `VERSION` | 帧头第 5 字节 | 必须完全一致；只有帧头布局变化才 +1 |
| 语义 `PROTO_VERSION` | HELLO 第 2 字节 | relay 接受 `>= PROTO_MIN_VERSION`；当前 1 |

新增能力时的做法：

1. **加可选字段**：直接加到 JSON 对象尾部，不改版本；读取方忽略未知键。
2. **加新帧类型**：选未占用编号；旧端收到会回 `ERROR` 或忽略，调用方必须能降级。
   需要确认对端支持时，先看 `HELLO_ACK` 的 `relay_proto`，或先发一个探测帧看回什么。
3. **改已有字段语义 / 删字段 / 改帧头**：属于破坏性变更，`PROTO_VERSION` +1，
   并把 `PROTO_MIN_VERSION` 抬到新值。旧端连上来会收到 `AUTH_FAIL`。
4. **二进制帧**（`DIAG_DATA`、`FILE_DATA`、`STORAGE_DATA`）只能整体理解，
   不能靠“忽略未知字段”兼容；格式变化必须提升版本。

升级顺序建议：先 relay，后 agent。新 relay 接受旧 agent（`proto` 低于自己），
新 agent 连旧 relay 也能跑，只是日志会提示 `relay protocol N < local M`。
两端都不需要“精确同版本”。

回归：`rtunnel/tests/compat_e2e.py` 验证版本区间、拒绝低版本、接受高版本、
帧头不一致断开；`rtunnel/tests/protocol_doc_test.py` 校验本文档覆盖了所有帧类型。

## 10. 调试

- relay / agent 加 `-v` 会打印每帧的 `role/type/sid/len`。
- 抓包后按第 1 节手工解帧即可；payload 是 UTF-8 JSON，`str` 前缀去掉就是原文。
- `SYSINFO` 的 `caps` 是排查“功能为什么灰掉”的第一入口：能力位、白名单、配置来源都在里面。
- 管理台的“诊断包”包含最近日志和 `sysinfo.json`，现场问题优先要这个。

## 11. 部署边界

协议本身不定义 relay 之间的状态同步。每个 relay 的管理库（`--admin-db`）是独立的：
账号、权限组、Token、审计、分发任务、采样配置都不跨实例。多 relay 只能解决链路冗余，
不能当成管理高可用；设备切换到另一个 relay 后，原 relay 的管理台不会再看到它。

部署约束见 `rtunnel/README.md`「多 relay 与故障切换 / 管理边界」：不要共享 SQLite 文件，
不要让负载均衡轮询管理请求，需要统一管理时只保留一个 relay 承载管理面。

