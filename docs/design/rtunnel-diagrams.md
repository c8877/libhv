# rtunnel 图表与表格总览

本文档只放图：拓扑、业务流程、时序与状态（Mermaid + ASCII 各一份）。
协议帧表、错误码、配置项、状态与参数以 `rtunnel.md` 为准，这里不再复制一份，避免两处不同步。

目录：

1. 业务系统拓扑图
2. 业务拓扑图（模块）
3. 流程图（设备 Web 访问）
4. 时序图（登录 → 设备详情 → Web/Shell → 改能力）
5. 状态图（连接 / 会话）

---

## 1. 业务系统拓扑图

### 1.1 Mermaid

```mermaid
flowchart LR
    U[运维/开发浏览器] -->|HTTPS/HTTP 9443| ADM[B: admin 管理台]
    ADM -->|RTNP client 本机回环| REL[B: relay 18443]
    A[A: rtunnel-client] -->|RTNP/TLS| REL
    REL <-->|RTNP/TLS 长连接| C1[C: rtunnel-agent board01]
    REL <-->|RTNP/TLS 长连接| C2[C: rtunnel-agent 其它设备]

    A -->|SOCKS5 11080| DEV[开发工具 ssh/curl]
    A -->|本地端口 16022| DEV
    ADM -->|Shell / Web 代理| C1
    C1 -->|pty / 127.0.0.1:port| SVC[(设备本地服务)]
    C2 -->|pty / 127.0.0.1:port| SVC2[(设备本地服务)]

    ADM --- DB[(SQLite: users/sessions/devices)]
    ADM --- FS[(文件目录 / 证书台账)]
```

### 1.2 ASCII

```
                   ┌──────────────────────────────┐
                   │       浏览器 / 开发者         │
                   └───────┬───────────────┬──────┘
             HTTP/WS 9443  │               │ SOCKS5 11080 / 本地端口
                           ▼               ▼
                 ┌──────────────────┐   ┌──────────────────┐
                 │  B: admin 管理台  │   │  A: client       │
                 │  账号/API/代理/终端│   │  socks5/forward  │
                 └───┬──────────┬───┘   └────────┬─────────┘
                     │          │                │
             SQLite  │          │ RTNP(本机回环) │ RTNP/TLS
                     ▼          ▼                ▼
                 ┌──────┐   ┌──────────────────────────┐
                 │ DB   │   │      B: relay 18443      │
                 └──────┘   └───────┬──────────┬───────┘
                                    │ RTNP/TLS │ RTNP/TLS
                                    ▼          ▼
                          ┌─────────────┐  ┌─────────────┐
                          │ C: board01  │  │ C: 其它设备  │
                          │  agent      │  │  agent      │
                          └──────┬──────┘  └──────┬──────┘
                                 │ pty / 127.0.0.1:port
                                 ▼
                          ┌─────────────┐
                          │ 设备本地服务 │
                          └─────────────┘
```

---

## 2. 业务拓扑图（模块）

### 2.1 Mermaid

```mermaid
flowchart TB
    subgraph B[B 端]
        UI[Vue 管理台<br/>Vite + Element Plus]
        API[AdminServer<br/>会话/角色/审计]
        PROXY[设备 Web 代理<br/>HTTP 流式 + WS]
        TERM[Web 终端<br/>xterm + WS]
        REL[RelayServer<br/>连接注册/路由/权限组/流控]
        CACHE[(sysinfo / caps<br/>内存缓存)]
        DB[(SQLite)]
        UI --> API
        API --> PROXY
        API --> TERM
        API --> DB
        API -->|callRelay| REL
        REL --- CACHE
    end
    subgraph A[A 端]
        CL[rtunnel-client]
        SOCKS[SOCKS5]
        FWD[本地端口转发]
        SPWN[按需 agent 拉起]
        CL --> SOCKS
        CL --> FWD
        CL --> SPWN
    end
    subgraph C[C 端]
        AG[rtunnel-agent]
        PTY[pty shell]
        TCP[TCP 连接]
        CFG[(rtunnel-agent.json<br/>allow/shell)]
        AG --> PTY
        AG --> TCP
        AG --> CFG
    end
    CL -->|RTNP| REL
    REL -->|RTNP| AG
    PROXY -->|OPEN| REL
    TERM -->|OPEN_SHELL| REL
```

### 2.2 ASCII

```
┌─────────────────────────────── B ────────────────────────────────┐
│  Vue 管理台 ──► AdminServer ──► SQLite(users/sessions/devices)     │
│                  │  │                                             │
│       ┌──────────┘  └────────────┐                                │
│       ▼                          ▼                                │
│  设备 Web 代理               Web 终端                             │
│  HTTP 流式 + WS              xterm + WS                           │
│       │                          │                                │
│       └──────────┬───────────────┘                                │
│                  ▼                                                │
│            RelayServer 18443  ◄── sysinfo / caps 缓存             │
└──────────────────┬────────────────────────────────────────────────┘
                   │ RTNP/TLS
        ┌──────────┴──────────┐
        ▼                     ▼
┌─────── A ───────┐   ┌─────── C ─────────┐
│ rtunnel-client  │   │ rtunnel-agent     │
│ ├ SOCKS5        │   │ ├ pty shell       │
│ ├ 本地端口转发  │   │ ├ TCP 127.0.0.1:p │
│ └ 按需 agent    │   │ └ agent.json      │
└─────────────────┘   └───────────────────┘
```

---

## 3. 流程图（设备 Web 访问）

### 3.1 Mermaid

```mermaid
flowchart TD
    A[浏览器请求 /device/devid/port/path] --> B{会话 Cookie 有效?}
    B -- 否 --> B1[401 / 回登录页]
    B -- 是 --> C{角色/设备归属允许?}
    C -- 否 --> C1[403 forbidden]
    C -- 是 --> D{设备在线?}
    D -- 否 --> D1[502 device offline]
    D -- 是 --> E[AdminServer 建 RTNP link]
    E --> F[OPEN devid port]
    F --> G{agent 校验 allow}
    G -- 拒绝 --> G1[502 open failed: port not allowed]
    G -- 允许 --> H{端口可连接?}
    H -- 否 --> H1[502 open failed: connect refused]
    H -- 是 --> I[转发 HTTP/WS]
    I --> J[响应流式返回浏览器]
    J --> K[审计/统计]
```

### 3.2 ASCII

```
浏览器 /device/{devid}/{port}/...
   │
   ├─ 无会话 ──────────────► 401 → 登录页
   ├─ 无权限/非本人设备 ───► 403 forbidden
   ├─ 设备离线 ────────────► 502 device offline
   ▼
AdminServer 建 RTNP link
   │
   ├─ OPEN(devid, port)
   ▼
agent 校验 allow 列表
   ├─ 不在列表 ────────────► 502 port not allowed
   ├─ 端口无服务 ──────────► 502 connect refused
   ▼
连接 127.0.0.1:port
   │
   ├─ HTTP：响应头到达即 chunked 流式回写
   └─ WS：二次握手后帧级双向转发
   ▼
浏览器（Cookie 前缀隔离、审计）
```

---

## 4. 时序图

### 4.1 Mermaid

```mermaid
sequenceDiagram
    autonumber
    participant U as 浏览器
    participant A as AdminServer(B)
    participant R as RelayServer(B)
    participant C as agent(C)
    participant S as 设备服务

    U->>A: POST /api/v1/login
    A-->>U: Set-Cookie rt_sid
    U->>A: GET /api/v1/agents/board01
    A->>R: 查询在线/流
    R-->>A: 设备信息
    A-->>U: 设备详情

    U->>A: GET /agents/board01/sysinfo?refresh=1
    A->>R: SYSINFO_REQ
    R->>C: SYSINFO_REQ
    C->>C: 采集 uname/CPU/内存/分区/网卡/证书
    C-->>R: SYSINFO(json)
    R-->>A: 缓存快照
    A-->>U: sysinfo + ts

    U->>A: GET /device/board01/80/
    A->>R: OPEN(devid=board01, port=80)
    R->>C: OPEN(port=80)
    C->>C: 校验 allow
    C->>S: connect 127.0.0.1:80
    S-->>C: 101/200
    C-->>R: OPEN_OK
    R-->>A: OPEN_OK
    A-->>U: 200 / 流式响应

    U->>A: PUT /agents/board01/caps {ports,shell}
    A->>R: CONFIG_SET(sid=1)
    R->>C: CONFIG_SET
    C->>C: 原子更新 + 写 rtunnel-agent.json
    C-->>R: CONFIG_ACK(sid=1)
    R-->>A: ACK
    A-->>U: 生效列表
```

### 4.2 ASCII

```
浏览器            AdminServer(B)        RelayServer(B)         agent(C)          设备服务
  │                    │                     │                    │                 │
  │ POST /login        │                     │                    │                 │
  │───────────────────►│                     │                    │                 │
  │◄── Set-Cookie ─────│                     │                    │                 │
  │                    │                     │                    │                 │
  │ GET /agents/board01│                     │                    │                 │
  │───────────────────►│── 查询 ────────────►│                    │                 │
  │◄── 设备详情 ───────│◄────────────────────│                    │                 │
  │                    │                     │                    │                 │
  │ GET /sysinfo?refresh=1                   │                    │                 │
  │───────────────────►│── SYSINFO_REQ ─────►│── SYSINFO_REQ ────►│                 │
  │                    │                     │                    │ 采集系统信息     │
  │                    │                     │◄── SYSINFO(json) ──│                 │
  │◄── sysinfo + ts ───│◄── 缓存快照 ────────│                    │                 │
  │                    │                     │                    │                 │
  │ GET /device/board01/80/                  │                    │                 │
  │───────────────────►│── OPEN(80) ────────►│── OPEN(80) ───────►│                 │
  │                    │                     │                    │── connect ─────►│
  │                    │                     │◄── OPEN_OK ────────│◄── 200 ─────────│
  │◄── 200 / 流式 ─────│◄── OPEN_OK ─────────│                    │                 │
  │                    │                     │                    │                 │
  │ PUT /caps{ports,shell}                   │                    │                 │
  │───────────────────►│── CONFIG_SET ──────►│── CONFIG_SET ─────►│                 │
  │                    │                     │                    │ 更新+写配置文件  │
  │                    │                     │◄── CONFIG_ACK ─────│                 │
  │◄── 生效列表 ───────│◄── ACK ─────────────│                    │                 │
```

---

## 5. 状态图

### 5.1 Mermaid（连接）

```mermaid
stateDiagram-v2
    [*] --> TCP_CONNECTED
    TCP_CONNECTED --> HELLO_SENT: 发 HELLO
    HELLO_SENT --> AUTH_SENT: HELLO_ACK
    HELLO_SENT --> CLOSED: 版本不匹配/5s 超时
    AUTH_SENT --> READY: AUTH_OK
    AUTH_SENT --> CLOSED: AUTH_FAIL/10s 超时
    READY --> DRAINING: 主动关闭
    DRAINING --> CLOSED: 帧写完/10s 超时
    READY --> CLOSED: 心跳超时/TCP 断
```

### 5.2 Mermaid（会话流）

```mermaid
stateDiagram-v2
    [*] --> OPENING
    OPENING --> OPEN: OPEN_OK
    OPENING --> CLOSED: OPEN_FAIL/RESET/15s 超时
    OPEN --> HALF_CLOSED: 任一端 CLOSE
    HALF_CLOSED --> CLOSED: 双向 CLOSE / 60s 超时
    OPEN --> CLOSED: RESET / 连接断开
    CLOSED --> [*]
```

### 5.3 ASCII

```
连接：
  [*] ──TCP──► TCP_CONNECTED ──HELLO──► HELLO_SENT
                                          │  HELLO_ACK
                                          ▼
                                      AUTH_SENT ──AUTH_OK──► READY
                                          │                    │
                               AUTH_FAIL/10s                 │ 主动关闭
                                          ▼                    ▼
                                       CLOSED ◄── 心跳超时 ── DRAINING ──10s──► CLOSED

会话：
  [*] ──OPEN──► OPENING ──OPEN_OK──► OPEN
                   │                   │
        OPEN_FAIL/RESET/15s     任一端 CLOSE
                   │                   ▼
                   │              HALF_CLOSED
                   │                   │ 双向 CLOSE / 60s
                   ▼                   ▼
                 CLOSED ◄──────────────┘
```

---

## 6. 其余表格

协议帧（§8.2）、错误码（§8.3）、状态机（§8.4）、配置项（§13）、
不变量与超时（附录 A）都在 `rtunnel.md`，本文档不重复维护。
