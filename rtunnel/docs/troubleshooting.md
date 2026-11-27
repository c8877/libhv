# 故障排查

先确认“在哪一段”：A→B 断、B→C 断、还是设备上服务没起来。relay 日志里每次认证失败、
每次 OPEN 拒绝都有一行原因码，比页面上的提示更具体。

## 连不上

| 现象 | 原因 / 处理 |
|---|---|
| A 端上线即断开 | `--token` 缺失或不是 `rtk-`；令牌停用/过期/超额；归属账号停用 |
| A 端报 `AUTH_FAIL` 但令牌看着没问题 | 令牌是另一个 relay 签发的。每台 relay 各自独立管理，r1 的 token 在 r2 无效 |
| `OPEN_FAIL: acl denied` | 接入账号未加入权限组，或组里没有该设备/端口。令牌只能收窄不能放大 |
| `OPEN_FAIL: shell disabled` | 目标 agent 没开 `--allow-shell` |
| C 端一直重连 | relay 地址/端口错（数据面端口，不是管理台 9443）；防火墙只放了管理端口 |
| C 端连上后 60s 被踢 | 心跳没通：中间设备掐长连接；查 `--stream-idle` 与 NAT 超时 |
| client 启动报 `bind: Address already in use` | 默认监听 1080；换 `--socks5 127.0.0.1:<port>`，或省略 `--socks5` 只做 forward |
| 多 relay 下设备“丢了” | 设备切到了另一个 relay，r1 管理台看不到它是预期行为；看它当前连的是哪台 |

## 握手/认证（管理台「测试连接」失败）

| 提示 | 含义 |
|---|---|
| `Connection refused/timeout` | 端口没开、防火墙、地址错 |
| `密码认证失败` | 账号或密码错；也可能是服务端禁用了密码登录 |
| `私钥认证失败` | 私钥格式/口令不对，或公钥没加到目标账号的 `authorized_keys` |
| `SSH 握手超时` | TCP 通但 SSH 没谈成：端口不是 SSH、被中间设备拦了、或算法不兼容。**扫描只看端口开着，扫到不等于能登录** |
| `主机密钥变了` | `known_hosts` 里记录的指纹和现在不一致。确认是重装/换机后，删掉 relay `<admin.db 同目录>/deploy/known_hosts` 里那一行再试 |
| `sudo: 需要密码` | 非 root 账号勾了 sudo 但走的是密钥登录，且该账号没有免密 sudo。改用密码登录，或给该账号配 `NOPASSWD` |

Dropbear 这类服务端连上后要等几百毫秒才发 banner，超时判断按**总时长**（20s）算，
不会因为 200ms 没数据就判死。如果仍然超时，用 `nc -v host 22` 看有没有 banner。

## 部署

| 现象 | 处理 |
|---|---|
| 预检报架构不匹配 | 包名里的架构必须等于目标 `uname -m`；aarch64 与 armv7l 不通用，换对应包 |
| 预检报不是 root 且没 sudo | 装到 `/opt/rtunnel/*` 并要接管 systemd 时必须 root 或勾 sudo；自定义前缀 + 只装文件可以不用 |
| 写 `/opt` 权限不足 | 目标机 `/opt` 只读，或用了只读文件系统；换前缀或 `--no-units` |
| `install.sh` 退出码 5 | 包校验失败（下载损坏/篡改），重新上传 |
| `install.sh` 退出码 6 | 版本降级被拒；确实要降级加 `--force` |
| 服务没起来（`SVC=inactive`） | 容器里没有 systemd：勾「只安装文件」；否则看 `journalctl -u rtunnel-*` |
| 上传失败 | 包超过 `file.max_upload_mb`（默认 32 MiB）或磁盘满；先清理或调大设置 |
| 扫不到主机 | 确认是私有网段、主机确实开着 SSH；没装 nmap 时用内置 TCP 扫描，被防火墙丢包的网段扫不出来；非 22 端口要在「端口」里改 |

## 管理台

| 现象 | 处理 |
|---|---|
| 打开是空白/404 | 没配 `--admin-docroot` 且二进制不是 embedded 版；用内嵌版或把 `web/dist` 部署过去 |
| 登录后立刻回登录页 | Cookie 被拒：管理台是 HTTPS 自签但浏览器没信任；或系统时间偏差过大 |
| 页面在但接口全 401 | 会话过期（默认 12h）或账号被停用；重新登录 |
| 菜单里没有某个功能 | 当前账号没有对应资源键。让管理员在「权限组」里加，不是页面 bug |
| 改了权限没生效 | 权限只影响新 OPEN，已建立的会话不动；断开重连再看 |
| 点设置页左侧分组跳回概览 | 旧版本 bug（锚点被 hash 路由当成导航），1.4.161 起已修 |
| 告警发了但页面没有 | 告警页读 `alert_events` 表，确认 relay 已升级；webhook 发送结果看 `/api/v1/alerts/status` |

## 设备本地页

| 现象 | 处理 |
|---|---|
| `http://设备:8080/` 打不开 | agent 没加 `--local-web`，或用的是 lite 包（noweb 构建不带页面） |
| 能打开但文件不能写 | `--file-rw` 没开，或配置里 `file_rw` 为 false；本地页只读是默认 |
| 对外开放后谁都能改 | 必须同时配 `--file-token` / `--svc-token`，同源检查只防浏览器跨站 |
| 服务列表空白 | 设备没有 systemd（`/run/systemd/system` 不存在），能力里 `caps.svc=false` |

## 其他

| 现象 | 处理 |
|---|---|
| 部署后 `undefined symbol: hv_version` | 用了共享库构建只拷了二进制；改静态构建，或一并部署 `libhv.so` |
| Web 终端显示「连接已关闭」 | 1.3.19 之前 `shell_relay_port` 不跟随 `--port`；升级 relay |
| 客户端列表 Shell 没反应 | client 没开 `--allow-shell`，或 devid 不是 `a:<user>` |
| 设备 Web 代理 503 | 并发上限 32，等前一批结束；不支持设备侧 HTTPS |
| 设备时间不对导致证书/令牌异常 | 设备无 RTC 时先同步时间（`ntpd`/手动），证书有效期按设备时钟判断 |
