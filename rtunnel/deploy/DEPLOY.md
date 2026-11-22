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
./build_mp135.sh                      # 产物 build/deploy/agent{,-web}/bin/rtunnel-agent
SYSROOT=/opt/st/myir-yf13x/4.0.4-snapshot/sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
RTUNNEL_ARCH=armv7l RTUNNEL_PKG_RUNNER="qemu-arm-static -L $SYSROOT" \
    bash rtunnel/deploy/package_agent.sh <版本>
```

`package_agent.sh` 用 qemu 跑目标二进制取 `--version` 自检，所以 `RTUNNEL_PKG_RUNNER`
必须给，否则版本对不上。产物：`build/deploy/packages/rtunnel-agent-{web,lite}-<版本>-armv7l.tar.gz`。

### 2. 推送安装

paramiko（`sshpass` 没装，用 python + paramiko 最省事）：

1. sftp 上传 tar.gz 到设备 `/tmp`，比对 sha256
2. 解到 `/tmp/rtunnel-pkg-<版本>`（`--strip-components=1`）
3. `sh <pkg>/install.sh --pkg-dir <pkg> --variant web --version <版本>`
4. `systemctl restart rtunnel-agent`

`install.sh` 只在配置不存在时写 `/opt/data/etc/rtunnel-agent.json`，升级不会覆盖现有能力配置；
旧版本目录保留，软链切到新版本，`--rollback` 可退回。

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

```bash
cd <repo 根目录>
cmake --build build_static --target rtunnel-relay -j8
```

替换流程（管理台静态目录同理）：

1. sftp 到 `/tmp/rtunnel-relay.new`、`/tmp/admin-dist.tar.gz`，比对 sha256
2. 管理台：解到 `admin/web.new` → `mv admin/web admin/web.bak-<ts>` → `mv admin/web.new admin/web`
3. 二进制：`cp -a rtunnel-relay rtunnel-relay.bak-<ts>` → `mv rtunnel-relay.new rtunnel-relay`
4. 停旧进程（SIGTERM 走 drain，等设备重连）→ `nohup ./rtunnel-relay $(cat relay.opts) >> relay.log 2>&1 < /dev/null &`

验证：

```bash
pidof rtunnel-relay
ss -ltn | grep -E ':18443|:9443'
curl -k -s -o /dev/null -w '%{http_code}\n' https://127.0.0.1:9443/
curl -k -s -o /dev/null -w '%{http_code}\n' https://127.0.0.1:9443/assets/<新 chunk>
ss -tn | grep ':18443'        # 设备应自动重连
```

管理台前端改动要先构建 `rtunnel/relay/admin/web/dist`，否则页面还是旧的。

### 文件仓库（OTA 分发用）

`upload_agent.sh` 里内置了测试环境的默认令牌（`ATK="${ATK:-atk-...}"`），直接跑即可；
轮换后改那一行。`/tmp` 脚本里的旧令牌都已失效，不要复用。

```bash
RT=https://192.168.1.133:9443 INSECURE=1 RTUNNEL_ARCH=armv7l \
    bash rtunnel/deploy/upload_agent.sh <版本>
```

不想用令牌时直接 scp 到 `guest@192.168.1.133:/tmp/rtunnel/admin/files/agent/`，
列表按目录实时扫描，效果一样。



## 部署后怎么报

只报结论：版本、服务状态、隧道是否 ESTAB、线程/RSS。不要贴复述性的清单，
不要解释本来就在机器上的路径。没有实际执行就说没执行，不要用"需要你提供……"兜底。
