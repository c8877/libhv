# 构建、测试与发布

## 构建

```bash
# 仓库根目录（libhv/）；产物在 build/bin/rtunnel-*
./enp_build.sh --static --shared

# TLS / mTLS（relay 账号库也需要 OpenSSL）
./enp_build.sh --static --shared --with-openssl

# 只改后端时
cmake --build build --target rtunnel-relay -j8

# 内嵌前端的单文件 relay（构建期把 web/dist 压进二进制，约 +0.8 MB）
cmake .. -DRTUNNEL_ADMIN_WEB=ON
cmake --build build_static --target rtunnel-relay-embedded -j8

# 设备离线的交叉编译
./build_mp135.sh
```

管理台与设备本地页（`relay/admin/web`）：

```bash
cd rtunnel/relay/admin/web
npm install
npm run check:ui      # 组件规约检查（自建 Element 能力会失败）
npm run check:docs    # 接口文档与 admin_routes.cpp 对表
npm run dev           # 本地开发，代理到 relay 管理端口
npm run build         # → dist/，配合 --admin-docroot
npm run build:local   # → rtunnel/agent/web/index.html（设备本地页，构建产物不要手改）
```

改页面必跑 `check:ui` + `build`；`build:local` 会调用后者，不用手抄步骤。

## 测试

### 数据面与端到端

```bash
make rtunnel_test                        # 明文
RTUNNEL_TLS=1 make rtunnel_test          # TLS
RTUNNEL_MTLS=1 make rtunnel_test         # mTLS（证书 SAN 必须等于 devid，含负向用例）
RTUNNEL_UDS=1 make rtunnel_test          # 本地入口走 UDS（需 -DENABLE_UDS=ON）
```

```bash
python3 rtunnel/tests/e2e_failover.py      # 双 relay 故障切换
python3 rtunnel/tests/e2e_alerts.py        # 告警（本地 webhook sink）
bash    rtunnel/tests/ota_e2e.sh           # OTA 安装/重启/版本确认 + 错误架构拒绝
bash    rtunnel/tests/dist_scope_e2e.sh    # 分发/OTA 的设备范围授权
bash    rtunnel/tests/sysinfo_e2e.sh       # 挂载字段全量/增量 + 离线缓存
bash    rtunnel/tests/net_e2e.sh           # 网络/IPC 快照、进程归属、缓存与回退
bash    rtunnel/tests/proc_e2e.sh          # 进程列表/详情/信号与 device.proc 权限
bash    rtunnel/tests/storage_e2e.sh       # 目录树按路径/层级、子项求和、越界校验
python3 rtunnel/tests/stream_idle_e2e.py   # 流空闲回收：闲置 RESET、活跃流保留
python3 rtunnel/tests/permission_e2e.py    # 权限：混合组端口范围、WS 按会话判定身份
python3 rtunnel/tests/usage_e2e.py         # 用量：流量/会话/在线计量、日聚合
python3 rtunnel/tests/tenant_e2e.py        # 多租户：隔离、停用、用量归集、套餐
python3 rtunnel/tests/bandwidth_e2e.py     # 带宽整形：限速生效、不丢数据
python3 rtunnel/tests/billing_e2e.py       # 费用：价目、金额、结算单状态机
python3 rtunnel/tests/admin_embedded_e2e.py # 内嵌前端：gzip、回退与优先级
python3 rtunnel/tests/packages_e2e.py      # 部署包：打包→发布→自举→篡改拒绝→回滚
python3 rtunnel/tests/exec_e2e.py          # 受控执行：白名单、分流、超时、plan/apply/verify
python3 rtunnel/tests/events_e2e.py        # 事件流 SSE 与分钟指标：续传、隔离
python3 rtunnel/tests/playbook_e2e.py      # 自愈：自动执行/超限转审批/冷却/越权
python3 rtunnel/tests/ai_e2e.py            # AI 助手：配置、问答、提议作业、越权、上游报错
python3 rtunnel/tests/ai_session_e2e.py    # 会话落库、翻页、改名删除、重启后仍在
python3 rtunnel/tests/mcp_e2e.py           # OpenAPI 与 MCP：工具清单、只读默认、审计标记
python3 rtunnel/tests/compat_e2e.py        # HELLO 版本区间、低版本拒绝、高版本兼容
python3 rtunnel/tests/protocol_doc_test.py # 协议文档覆盖率（帧类型/常量）
python3 rtunnel/tests/deploy_e2e.py        # 管理台部署：私有 sshd 上真实 SSH/SFTP/install.sh
build/bin/rtunnel-endpoint-test            # --server 地址解析单测
```

`deploy_e2e.py` 会在随机端口起一个只认 `/tmp` 下 `authorized_keys` 的私有 sshd，
不碰 `~/.ssh`，也不需要 root；覆盖 RSA/ed25519 私钥、密码错、主机密钥变化、
服务端慢发 banner、扫描双引擎、包路径越界。

### UI（Playwright）

```bash
rtunnel/tests/ui/run-ui.sh                 # 全量（管理台 + 设备本地页）
rtunnel/tests/ui/run-ui.sh admin/deploy    # 单套
rtunnel/tests/ui/run-ui.sh list            # 列出全部套件
```

用例放 `rtunnel/tests/ui/{admin,local}/`，用 `lib/harness.js` 的 `check()`，
新增后登记到 `run-ui.sh` 的 `SUITES`。不要再把 UI 测试写到 `/tmp`——那里不会被回归。

覆盖：登录/权限、导航与侧栏排版、设备列表与详情、文件/服务/存储、部署、告警、审计、
设置页锚点、助手、表格排序分页、表单校验、内嵌页与本地页。

## 打包

三个组件同一套包格式，解压出一层 `rtunnel-<component>-<ver>-<arch>/`：

```
bin/            版本化二进制 + 软链 + agent-boot-check.sh
etc/            默认配置
service/        systemd unit（可选）
install.sh      只做文件操作：校验 manifest/版本/SHA256SUMS → 原子切软链
manifest.json   组件/版本/架构/变体
SHA256SUMS
```

```bash
./rtunnel/deploy/build_packages.sh <ver> --arch x86_64
./rtunnel/deploy/package_agent.sh <ver>                 # agent 的 lite / web 变体

# 交叉编译（ARM）：qemu 跑目标二进制自检，架构必须显式给
RTUNNEL_DEPLOY_DIR=build/deploy_mp135 RTUNNEL_ARCH=armv7l \
RTUNNEL_PKG_RUNNER="qemu-arm-static -L <sysroot>" \
  ./rtunnel/deploy/package_agent.sh <ver>
```

`install.sh` 的退出码：2 参数错、3 权限不足、5 包校验失败、6 版本降级（`--force` 才装）。
`--start/--restart` 才动 systemd，重启失败自动回滚一次；没有 systemd 时改调
`restart.sh`，不会“假装启动成功”。容器里用 `--no-units`（`RTUNNEL_INSTALL_NO_UNITS=1`）
完全不碰 unit。

## 发布与自举

```bash
# 发布到文件仓库（幂等：同 sha256 跳过）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rtunnel/deploy/publish.sh --component all --version <ver> --arch x86_64

# 目标机自举：取 latest → 下载 → 校验 → 解包 → install.sh
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk \
  ./rtunnel/deploy/rt-bootstrap.sh --component relay --version latest \
    --arch x86_64 --start

# 日常
/opt/rtunnel/relay/install.sh --status
/opt/rtunnel/relay/install.sh --rollback --restart

# 无 systemd（容器 / /tmp）
/tmp/rtunnel/relay/restart.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
/tmp/rtunnel/relay/install-boot.sh --prefix /tmp/rtunnel/relay --log /tmp/rtunnel/relay.log
```

仓库布局：

```
packages/<component>/<ver>/<arch>/<pkg>.tar.gz
packages/<component>/latest/<arch>.json          # agent 为 <arch>-<variant>.json
packages/<component>/<ver>/<arch>/manifest.json  # 包内副本
```

- `publish.sh` 需要 `atk-` 令牌（`ATK_FILE` 0600，不写进脚本或仓库）。
- 版本号取二进制 `--version`，架构取 `uname -m`；交叉编译用 `RTUNNEL_ARCH` 指定，
  两者不一致 `install.sh` 会拒绝。
- 测试环境直推（不经文件仓库）见 [`../deploy/DEPLOY.md`](../deploy/DEPLOY.md)。
