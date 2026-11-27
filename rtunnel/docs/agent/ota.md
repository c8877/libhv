# Agent OTA 升级

OTA 不另建传输通道：部署包走「文件上传 + 文件分发」落到设备 `--file-dir`，分发完成后
relay 下发安装指令，统一记录在 `distribution_tasks`（`kind: "ota"`）。任务在管理台
「文件与升级 → Agent 升级」和「任务」页查看。

## 流程

1. **打包**：`deploy/package_agent.sh` 生成 `rtunnel-agent-lite-<ver>-<arch>.tar.gz` 与
   `rtunnel-agent-web-<ver>-<arch>.tar.gz`。解压目录 `rtunnel-agent-<ver>-<arch>/`，
   包里固定 `bin/rtunnel-agent`、`bin/agent-boot-check.sh`、`bin/rtunnel-client`、
   `etc/rtunnel-agent.json`、`service/rtunnel-agent.service`、`manifest.json`、`SHA256SUMS`。
   variant 只体现在文件名与 manifest；版本取二进制 `--version`，架构取 `uname -m`
   （交叉编译用 `RTUNNEL_ARCH`）。
2. **发布**：`deploy/publish.sh --component agent --variant web` 用 `rtunnel-client repo`
   上传到 `packages/agent/<版本>/<架构>/`（需要 atk）。`/ota` 页按文件名递归搜索该目录，
   解析 variant/版本/架构，并按架构过滤设备。
3. **传输**：复用 `FILE_BEGIN/DATA/END`（0x1D-0x1F），落盘设备侧
   `packages/agent/<版本>/<架构>/xxx.tar.gz`。
4. **校验**：relay 发 `OTA_APPLY`（0x30，JSON：path/version/size/sha256）；agent 校验大小与
   sha256 → 解压 → 解包到 `.ota/`（拒绝 `..`、绝对路径、符号链接）→ manifest（版本/架构）与
   `--version` 自检（`verifying`）→ ACK `verified`（`installing`）。
5. **安装**：agent 执行包内 `install.sh`：装版本化二进制与 boot-check、更新 unit、配置不存在时
   安装默认配置、写 `state/previous` 与 `state/pending`，最后原子切
   `bin/rtunnel-agent -> rtunnel-agent-<variant>-<ver>`。
6. **重启**：ACK `installed`，等 ACK 写出后排队
   `systemctl --no-block restart rtunnel-agent.service` 并退出；unit 的 `Restart=always` 兜底
   （`confirming`）。
7. **确认**：新 agent 启动时 `ExecStartPre=agent-boot-check.sh` 计数；AUTH_OK 后清 pending、
   上报 SYSINFO（含版本与 `caps.arch`），relay 在 `confirming` 阶段比对目标版本 → `success`。

状态：`scheduled → queued → running → verifying → installing → confirming → success`；
失败分支 `failed / timeout / conflict / unsupported / canceled`。重试沿用分发任务的退避与
截止时间。

## 失败与回滚

| 情况 | 结果 |
|---|---|
| 包校验/解包/`--version` 自检失败 | 不动现场，旧版继续跑 |
| `install.sh` 失败 | 软链还没切换（切换是最后一步），旧版继续跑 |
| 新版本连续 3 次没起来，或每次启动 45s 内没连上 relay | `agent-boot-check.sh` 把软链切回 `state/previous`，任务显示 `version not confirmed` |
| 手动回滚 | `/opt/rtunnel/agent/install.sh --rollback`，然后重启服务 |

设备只保留当前和上一个二进制（回滚只需要这两个），更早的版本安装时清理。
查看当前/上一版本：`/opt/rtunnel/agent/install.sh --status`。

## 前置条件与限制

- 配置里 `"ota": true`（部署包自带 `/opt/data/etc/rtunnel-agent.json` 默认开启）。
- 只对常驻 agent 生效；按需 agent（`a:<user>`）拒绝。
- `/opt/rtunnel/agent/bin` 和 `/opt/data/etc` 必须可写。
- 升级会重启 agent，隧道与 shell 会断。
- 版本确认超时 `ota.apply_timeout_s`（默认 180s）。
- 只接受部署包（`.tar.gz`）。开发阶段不提供旧二进制/旧目录布局的自动迁移：
  统一按下面的布局安装，旧开发机直接重装。

## 首次安装

```bash
# 先写身份与 relay 地址
mkdir -p /opt/data/etc
cat > /opt/data/etc/rtunnel-agent.args <<'EOF'
AGENT_ARGS=--devid board01 --server 10.0.0.10:18443 --allow 22
EOF

# 解包并安装（--start 首次安装会 enable --now）
tar -xzf rtunnel-agent-lite-<ver>-<arch>.tar.gz
sh rtunnel-agent-<ver>-<arch>/install.sh \
  --pkg-dir rtunnel-agent-<ver>-<arch> \
  --variant lite --start
```

设备布局固定为 `/opt/rtunnel/agent`，unit 不解析额外参数：
`/opt/data/etc/rtunnel-agent.args` 只提供 `AGENT_ARGS`，能力开关在
`/opt/data/etc/rtunnel-agent.json`。

## 打包命令

```bash
# lite + web 两个包（版本默认取二进制 --version，架构默认 uname -m）
./deploy/package_agent.sh 1.4.9

# 交叉编译（ARM）：qemu 跑目标二进制自检，并显式指定架构
RTUNNEL_DEPLOY_DIR=build/deploy_mp135 RTUNNEL_ARCH=armv7l \
RTUNNEL_PKG_RUNNER="qemu-arm-static -L <sysroot>" \
./deploy/package_agent.sh 1.4.9

# 上传（架构要与打包时一致）
RT=https://relay:9443 ATK_FILE=/etc/rtunnel/atk RTUNNEL_ARCH=armv7l \
  ./deploy/publish.sh --component agent --variant web --version 1.4.9 --arch armv7l
```

容器或临时目录试装时加 `RTUNNEL_INSTALL_NO_UNITS=1`（或 `--no-units`），
`install.sh` 完全不碰 `/etc/systemd/system` 和 `systemctl`。

## API

`POST /api/v1/distributions` 增加 `kind: "ota"` 与 `version`、`force` 字段；
查询复用 `GET /api/v1/distributions?kind=ota` 与 `.../summary`；
重试/取消复用 `POST /api/v1/distributions/:id/retry|cancel`。
