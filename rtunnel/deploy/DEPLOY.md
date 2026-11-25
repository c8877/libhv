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

### 方式 B：直接替换（无 systemd / 临时排障）

替换流程：

1. sftp 到 `/tmp/rtunnel-relay.new`，比对 sha256
2. `cp -a rtunnel-relay rtunnel-relay.bak-<ts>` → `mv rtunnel-relay.new rtunnel-relay`
3. 停旧进程（SIGTERM 走 drain，等设备重连）→ `nohup ./rtunnel-relay $(cat relay.opts) >> relay.log 2>&1 < /dev/null &`

验证：

```bash
pidof rtunnel-relay
ss -ltn | grep -E ':18443|:9443'
curl -k -s -o /dev/null -w '%{http_code}\n' https://127.0.0.1:9443/
ss -tn | grep ':18443'        # 设备应自动重连
```

管理台前端改动要先构建 `rtunnel/relay/admin/web/dist`，否则页面还是旧的。
内嵌版带 `--admin-docroot` 时仍以磁盘为准（磁盘缺文件回退内嵌），排障时可临时指目录。

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




## 部署后怎么报

只报结论：版本、服务状态、隧道是否 ESTAB、线程/RSS。不要贴复述性的清单，
不要解释本来就在机器上的路径。没有实际执行就说没执行，不要用"需要你提供……"兜底。
