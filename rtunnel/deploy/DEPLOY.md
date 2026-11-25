# 部署手册（本机测试环境）

测试拓扑固定，按本文直接做，**不要用"需要你提供地址/令牌"结束任务**。

| 角色 | 主机 | 方式 |
|---|---|---|
| B：relay + 管理台 | 192.168.1.133 | `guest` 已配免密，`ssh`/`scp` 直连；程序、数据、日志全在 `/tmp/rtunnel`，没有 systemd |
| C：设备 mp135b | 192.168.1.55 | `root` 密码登录（未配密钥），本机没装 `sshpass`，用 python + paramiko |

板子密码不在仓库里（写入会被钩子拦截）。需要时先从本机 `/tmp` 的历史部署脚本取连接参数：

```bash
grep -l "192.168.1.55" /tmp/*.py    # deploy_board.py / probe_board.py 等
```

直推模式**不需要** `RT` / `ATK`。只有走「上传文件仓库 → 管理台 OTA 分发」时才需要
API Token，见 `README.md` 的「Agent OTA 升级」。

## C：设备侧

### 1. 交叉编译 + 打包

```bash
cd <repo 根目录>
# 设备本地页与设备详情共用 DeviceView；改过前端后先生成单文件 HTML
(cd rtunnel/relay/admin/web && npm run build:local)
./build_mp135.sh                      # 产物 build/deploy/agent{,-web}/bin/rtunnel-agent
SYSROOT=/opt/st/myir-yf13x/4.0.4-snapshot/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
RTUNNEL_ARCH=armv7l RTUNNEL_PKG_RUNNER="qemu-arm-static -L $SYSROOT" \
    bash rtunnel/deploy/package_agent.sh <版本>
```

`package_agent.sh` 用 qemu 跑目标二进制取 `--version` 自检，所以 `RTUNNEL_PKG_RUNNER`
必须给，否则版本对不上。产物：`build/deploy/packages/rtunnel-agent-{web,lite}-<版本>-armv7l.tar.gz`
（解压目录是 `rtunnel-agent-<版本>-armv7l/`，包内固定 `bin/rtunnel-agent`）。

### 2. 推送安装

paramiko（`sshpass` 没装，用 python + paramiko 最省事）：

1. sftp 上传 tar.gz 到设备 `/tmp`，比对 sha256
2. 解到 `/tmp/rtunnel-pkg-<版本>`
3. `sh <pkg>/install.sh --pkg-dir <pkg> --variant web`
4. `systemctl restart rtunnel-agent`

`install.sh` 只在配置不存在时写 `/opt/data/etc/rtunnel-agent.json`，升级不会覆盖现有能力配置；
旧版本目录保留，软链切到新版本，`--rollback` 可退回。

或者走仓库自举（设备上有 `rtunnel-client` 时）：

```bash
RT=https://192.168.1.133:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rt-bootstrap.sh --component agent --variant web --version latest --arch armv7l --restart
```

### 3. 验证（必须做，别只看 install 返回 ok）

```bash
readlink /opt/rtunnel/agent/bin/rtunnel-agent        # 指向新版本
/opt/rtunnel/agent/bin/rtunnel-agent --version       # 版本号
systemctl is-active rtunnel-agent                    # active
cat /opt/rtunnel/agent/state/pending                 # 应为 cleared
pid=$(pidof rtunnel-agent); grep -E 'Threads|VmRSS' /proc/$pid/status
ss -tn | grep 18443                                  # 到 relay 的 ESTAB
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:10080/api/v1/health
```

### 4. 回滚

```bash
sh /opt/rtunnel/agent/install.sh --rollback && systemctl restart rtunnel-agent
```

## B：relay 侧

二进制必须是**静态构建**（`build_static`）：共享库版本只拷二进制会报
`undefined symbol: hv_version`。

推荐用内嵌前端的单二进制（前端编进可执行文件，换 relay 不会再出现“页面还是旧的”）：

```bash
cd <repo 根目录>
cmake -S . -B build_static -DRTUNNEL_ADMIN_WEB=ON -DBUILD_SHARED=OFF
cmake --build build_static --target rtunnel-relay-embedded rtunnel-agent rtunnel-client -j8
cmake --build build_static --target rtunnel-deploy -j8     # 组装 build/deploy
```

### 方式 A：部署包 + client 自举（推荐，可复现）

```bash
# 1) 打包：relay/client 统一入口，agent 由 package_agent.sh 生成
./rtunnel/deploy/build_packages.sh <版本> --arch $(uname -m)
./rtunnel/deploy/package_agent.sh <版本>

# 2) 发布到文件仓库（幂等，可重复执行）
RT=https://192.168.1.133:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rtunnel/deploy/publish.sh --component all --version <版本> --arch $(uname -m)

# 3) 目标机自举（需要在机器上已有 rtunnel-client；首次 scp 这一个文件即可）
RT=https://192.168.1.133:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rt-bootstrap.sh --component relay --version latest --arch $(uname -m) --start
```

`/etc/rtunnel/atk` 是发布/下载共用的 atk- 令牌文件（0600，第一行）。发布机需要 `repo.write`，
目标机只需要 `repo.read`。

仓库位置：

```
<admin-file-dir>/packages/relay/<ver>/<arch>/rtunnel-relay-<ver>-<arch>.tar.gz
<admin-file-dir>/packages/relay/latest/<arch>.json      # {"version","file","sha256"}
```

### 方式 B：无 systemd（容器 / `/tmp` 手工部署，B 机器就是这种）

没有 systemd 时 `install.sh --no-units` 只装文件，启动交给包内的 `restart.sh`。
以 B（`guest@192.168.1.133`，前缀 `/tmp/rtunnel/relay`）为例，完整可复现流程：

```bash
# 0) 本机构建 + 打包（内嵌前端版）
cmake -S . -B build_static -DRTUNNEL_ADMIN_WEB=ON -DBUILD_SHARED=OFF
cmake --build build_static --target rtunnel-relay-embedded rtunnel-client -j8
cmake --build build_static --target rtunnel-deploy -j8
./rtunnel/deploy/build_packages.sh <版本> --arch x86_64

# 1) 首次：只需要把一个 rtunnel-client 放进目标机
scp rtunnel-client-<版本>-x86_64.tar.gz guest@B:/tmp/
# 解出 bin/rtunnel-client，写 /tmp/rtpkg/tok2（atk-，0600）

# 2) 发布（在目标机本地执行，走 127.0.0.1，避免自签证书）
RT=https://127.0.0.1:9443 ATK_FILE=/tmp/rtpkg/tok2 \
  RTUNNEL_CLIENT=/tmp/rtpkg/bin/rtunnel-client \
  ./publish.sh --component relay --version <版本> --arch x86_64 --dir /tmp/rtpkg --insecure

# 3) 自举安装（无 systemd）
RTUNNEL_CLIENT=/tmp/rtpkg/bin/rtunnel-client ./rt-bootstrap.sh \
  --component relay --version latest --arch x86_64 \
  --prefix /tmp/rtunnel/relay --no-units \
  --rt https://127.0.0.1:9443 --atk-file /tmp/rtpkg/tok2 --insecure

# 4) 参数文件：$PREFIX/etc/relay.env（RELAY_ARGS="..."），内嵌版不要 --admin-docroot
cat > /tmp/rtunnel/relay/etc/relay.env <<'EOF'
RELAY_ARGS="--host 0.0.0.0 --port 18443 --admin 0.0.0.0:9443 \
--admin-token <token> --admin-db /tmp/rtunnel/admin.db \
--admin-ca-cert /tmp/rtunnel/admin/ca.crt --admin-ca-key /tmp/rtunnel/admin/ca.key \
--admin-cert-dir /tmp/rtunnel/admin/certs --admin-file-dir /tmp/rtunnel/admin/files \
--verbose --admin-tls-selfsigned"
EOF

# 5) 启动/重启（同一命令，可重复执行）
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log

# 6) 开机自启 + 存活看护（无 systemd 时唯一可靠的方式）
/tmp/rtunnel/relay/install-boot.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
```

`restart.sh` 的规则：`pidof rtunnel-relay` 找进程（不用路径匹配，兼容相对/绝对路径启动）→
SIGTERM 等最多 15s 让 drain 完成 → `nohup $PREFIX/bin/rtunnel-relay $RELAY_ARGS` →
等 2s 检查端口；启动前会把参数文件（含 `--admin-token`）改成 0600。
退出码 3 启动失败、4 端口未监听。

`install-boot.sh` 给**当前用户**装两条 crontab：`@reboot` 启动、`*/5` 分钟进程不在就拉起；
重复执行只替换自己的标记行。`--print` 只打印不落盘，`--remove` 移除。
`install.sh --boot` 会在安装时一并调用它（`rt-bootstrap.sh --boot` 可透传）。

验证：

```bash
/tmp/rtunnel/relay/bin/rtunnel-relay --version      # 与包版本一致
/tmp/rtunnel/relay/install.sh --status              # current / previous
curl -sk -o /dev/null -w '%{http_code}\n' https://127.0.0.1:9443/   # 内嵌前端 200
ss -ltn | grep -E ':18443|:9443'
curl -sk -H "Authorization: Bearer $ATK" https://127.0.0.1:9443/api/v1/agents
```

回滚：`/tmp/rtunnel/relay/install.sh --rollback --prefix /tmp/rtunnel/relay`，再跑一次
`restart.sh`（`--rollback` 只切软链，不启动进程）。

管理台前端不需要单独部署：内嵌版已经把 `web/dist` 编进二进制，误加 `--admin-docroot`
会以磁盘为准，反而可能盖掉新前端。

### 文件仓库（OTA 分发用）

`upload_agent.sh` 里内置了测试环境的默认令牌（`ATK="${ATK:-atk-...}"`），直接跑即可；
轮换后改那一行。`/tmp` 脚本里的旧令牌都已失效，不要复用。

```bash
RT=https://192.168.1.133:9443 INSECURE=1 RTUNNEL_ARCH=armv7l \
    bash rtunnel/deploy/upload_agent.sh <版本>
```

新流程用 `rtunnel-client repo` 直传，不依赖 curl：

```bash
RT=https://192.168.1.133:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rtunnel/deploy/publish.sh --component agent --variant web \
    --version <版本> --arch armv7l --insecure
```

上传单文件受 `file.max_upload_mb`（默认 32 MiB）限制；包更大时先在「系统设置」调大。

### 从 1.4.105 及更早的 agent 迁到统一布局

旧 agent 的 manifest 校验要求 `binary = bin/rtunnel-agent-<variant>-<ver>`，直接推新包会被
`manifest binary mismatch` 拒绝。打一个迁移包（多带一份旧名字的二进制，manifest 指过去），
推一次即可，之后都走标准布局：

```bash
# 打迁移包：只有 -mig 后缀的产物，不影响标准包
RTUNNEL_ARCH=armv7l RTUNNEL_LEGACY_MANIFEST=1 \
RTUNNEL_PKG_RUNNER="qemu-arm-static -L <sysroot>" \
  ./rtunnel/deploy/package_agent.sh <版本>

# 上传（client repo put，publish.sh 只认标准文件名）
rtunnel-client repo put --rt $RT --atk-file $ATK_FILE --insecure \
  --dir packages/agent/<版本>/armv7l \
  --file rtunnel-agent-web-<版本>-armv7l-mig.tar.gz

# 建 OTA 任务：file 必须是仓库相对路径，dir 由 file 推导
curl -sS -X POST -H "Authorization: Bearer $ATK" -H 'Content-Type: application/json' \
  -d '{"kind":"ota","version":"<版本>",
       "file":"packages/agent/<版本>/armv7l/rtunnel-agent-web-<版本>-armv7l-mig.tar.gz",
       "devids":["<devid>"],"max_retries":1}' \
  "$RT/api/v1/distributions"
```

迁移包里的 `install.sh` 已经是新脚本，安装后 `bin/rtunnel-agent` 成为唯一入口，
下一次 OTA 用标准包即可。




## 部署后怎么报

只报结论：版本、服务状态、隧道是否 ESTAB、线程/RSS。不要贴复述性的清单，
不要解释本来就在机器上的路径。没有实际执行就说没执行，不要用"需要你提供……"兜底。
