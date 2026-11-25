# rtunnel 部署包与自举

目标：relay / agent / client 三个组件用同一套包格式、同一套命令完成「打包 → 发布 → 下载 → 安装 → 回滚」，
目标机上只要有一个 `rtunnel-client`，后续升级不再依赖 curl、scp 或手工分发。

```
 构建机                                relay 管理面                       目标机
┌──────────────┐   atk + repo.write  ┌──────────────────┐   atk + repo.read  ┌────────────────────┐
│ build_packages│ ─────────────────▶ │ <admin-file-dir>/ │ ◀──────────────── │ rtunnel-client     │
│ package_agent │    repo put        │   packages/...    │   repo get        │   repo get         │
│ publish.sh    │                    └──────────────────┘                   │     ↓ sha256 校验  │
└──────────────┘                                                           │   tar -xzf         │
                                                                           │   install.sh       │
                                                                           └─────────┬──────────┘
                                                                                     │ 包里自带
                                                                                     ▼ rtunnel-client
                                                                               下一跳仍可自举
```

## 1. 包结构

解压后一层目录，目录名与包名一致（agent 的文件名带 variant，目录不带）：

```
rtunnel-relay-<ver>-<arch>/
├── install.sh                     唯一入口，幂等，支持 --status/--rollback
├── manifest.json                  name/version/arch/binary
├── SHA256SUMS                     包内文件校验清单（install.sh 会核对）
├── bin/
│   ├── rtunnel-relay
│   ├── rtunnel-client             自举副本：下一版用它下载
│   ├── rt-bootstrap.sh            自举脚本副本（安装到 prefix）
│   ├── restart.sh                 无 systemd 环境的重启脚本（安装到 prefix）
│   └── install-boot.sh            无 systemd 的开机自启（crontab，--boot 时调用）
├── etc/relay.args.example         → /etc/rtunnel/relay.env（不覆盖现网）
└── service/rtunnel-relay.service  @PREFIX@/@STATE_DIR@/@FRONTEND_ARGS@ 由 install.sh 渲染

rtunnel-client-<ver>-<arch>/
├── bin/rtunnel-client
├── bin/rt-bootstrap.sh
├── etc/client.args.example        RT / ATK_FILE / SERVER / TOKEN
└── service/rtunnel-client-socks.service   可选常驻 SOCKS

rtunnel-agent-<ver>-<arch>/        （文件名 rtunnel-agent-<lite|web>-<ver>-<arch>.tar.gz）
├── bin/rtunnel-agent             统一入口名；variant 只写在 manifest
├── bin/agent-boot-check.sh
├── bin/rtunnel-client
├── bin/rt-bootstrap.sh
├── etc/rtunnel-agent.json
└── service/rtunnel-agent.service
```

- 压缩：`tar --format=ustar | gzip -9 -n`，同时产出 `<pkg>.tar.gz.sha256`
- agent 的文件名保留 `lite|web`，因为 OTA 页面按文件名选择变体；解压目录不带 variant（spec 要求的统一布局）
- `manifest.json` 里的 `binary` 固定为 `bin/rtunnel-agent` / `bin/rtunnel-relay` / `bin/rtunnel-client`

## 2. 仓库布局

```
<admin-file-dir>/
└── packages/
    ├── relay/
    │   ├── 1.4.130/x86_64/rtunnel-relay-1.4.130-x86_64.tar.gz(+.sha256)
    │   ├── 1.4.129/x86_64/…                       保留上一版，回滚不用找构建机
    │   └── latest/x86_64.json                     {"version","file","sha256"}
    ├── agent/
    │   ├── 1.4.130/armv7l/rtunnel-agent-{lite,web}-1.4.130-armv7l.tar.gz
    │   └── latest/armv7l-{lite,web}.json
    └── client/
        ├── 1.4.130/x86_64/…
        └── latest/x86_64.json
```

`latest` 指针最后上传，避免指向半截文件；包与指针在同一仓库，下载时用指针里的 sha256 兜底。

## 3. 命令链路

```bash
# 0) 构建（内嵌前端版 relay，单二进制交付）
cmake -S . -B build_static -DRTUNNEL_ADMIN_WEB=ON -DBUILD_SHARED=OFF
cmake --build build_static --target rtunnel-relay-embedded rtunnel-agent rtunnel-client -j8
cmake --build build_static --target rtunnel-deploy -j8

# 1) 打包（三个组件同构）
./rtunnel/deploy/build_packages.sh 1.4.130 --arch x86_64
./rtunnel/deploy/package_agent.sh 1.4.130          # agent 的 lite/web 变体

# 2) 发布（幂等：同 sha256 跳过）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rtunnel/deploy/publish.sh --component all --version 1.4.130 --arch x86_64

# 3) 目标机自举（relay 为例；client 首次需预置）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rt-bootstrap.sh --component relay --version latest --arch x86_64 --start

# 4) 日常运维
/opt/rtunnel/relay/install.sh --status
/opt/rtunnel/relay/install.sh --rollback --restart
# 无 systemd（容器 / /tmp 部署）：装完用包内的 restart.sh，读 $PREFIX/etc/relay.env
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
# 无 systemd 的开机自启与存活看护（crontab，重复执行只替换自己的行）
/tmp/rtunnel/relay/install-boot.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
```

## 4. 自举时序

```
目标机
  │ 第 0 跳：预置一个 rtunnel-client（镜像内置 / scp 一次）
  │ RT=https://relay:9443  ATK_FILE=/etc/rtunnel/atk
  ▼
rt-bootstrap.sh --component relay --version latest
  ├─ client repo get packages/relay/latest/<arch>.json      → version/file/sha256
  ├─ client repo get packages/relay/<ver>/<arch>/<pkg>.tar.gz --resume
  ├─ sha256 与 latest 指针比对（失败退出 5）
  ├─ tar -xzf → rtunnel-relay-<ver>-<arch>/
  └─ install.sh --pkg-dir <dir> --prefix /opt/rtunnel/relay --start
       ├─ manifest 架构/版本自检 + SHA256SUMS 核对
       ├─ 版本化二进制 → bin/rtunnel-relay-<ver>，原子切换软链
       ├─ 自举副本 bin/rtunnel-client、install.sh 副本
       ├─ unit 渲染并 enable，--start/--restart 才动服务
       └─ 重启后健康检查失败 → 自动回滚一次（退出 7）
```

安装布局（三个组件一致）：

```
/opt/rtunnel/<component>/
├── bin/<component>-<ver>      版本化实体
├── bin/<component> -> …       当前版本
├── bin/rtunnel-client         自举副本（relay/agent 包）
├── state/previous             上一版，供 --rollback
├── install.sh                 日常 --status/--rollback
└── service/<unit>
/etc/rtunnel/relay.env         只在新装时写示例
/etc/rtunnel/client.env
/opt/data/etc/rtunnel-agent.{json,args}   设备数据分区（不变）
```

## 5. install.sh 契约

```
install.sh --pkg-dir DIR [--prefix P] [--start|--restart] [--no-units] [--boot] [--force]
install.sh --rollback [--prefix P] [--restart]
install.sh --status   [--prefix P]
```

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 参数错误 |
| 3 | 权限不足（标准路径需要 root） |
| 4 | 缺文件 / 无可回滚版本 |
| 5 | 包校验失败（架构 / 版本 / SHA256SUMS） |
| 6 | 降级且未加 `--force` |
| 7 | 重启后健康检查失败（已尝试回滚） |

硬规则：

1. **只做文件操作**：装完不隐式 stop/start；`--start` 只用于首次 `enable --now`
2. **先校验后落盘**：架构、`--version` 自检、`SHA256SUMS` 全过才切换软链
3. **原子切换**：先建 `.tmp` 软链再 `mv -f` 覆盖，失败保留旧链接
4. 只保留当前与上一版，回滚永远有目标

## 6. 安全边界

- 发布令牌只需 `repo.write`，下载令牌只需 `repo.read`；建议单独建「发布机」账号
- atk 只从 `--atk-file`（0600）或 `ATK` 环境变量读取，不入包、不进脚本默认值
- 仓库是可变存储，sha256 只能防传输损坏，不能防仓库被改；需要防篡改时在 manifest 上加
  ed25519 签名（公钥编进 client，`install.sh --require-signature`），私钥留在发布机
- 上传单文件受 `file.max_upload_mb`（默认 32 MiB）限制；大包先在「系统设置」调大

## 7. 已知边界

- 不做 deb/rpm、不做跨架构自动编译（`RTUNNEL_ARCH` + `RTUNNEL_PKG_RUNNER` 手工交叉）
- 不做灰度编排：沿用管理台的「分发任务」按设备批次推送
- agent OTA 仍按文件名区分 lite/web；包内布局已统一，OTA 侧只校验 `binary` 字段
