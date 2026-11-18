#!/bin/sh
# rtunnel-agent 安装/升级脚本。
# 只做文件操作和 unit 的 enable/disable，绝不 stop/start/restart 服务：
# OTA 由 agent 在 ACK 之后自己排队重启，手工安装用 --start。
#
#   install.sh --pkg-dir DIR --variant lite|web --version VER [--prefix DIR]
#   install.sh --rollback [--prefix DIR]
#   install.sh --status [--prefix DIR]
set -eu

PREFIX=/opt/rtunnel/agent
CONF_DIR=${RTUNNEL_CONF_DIR:-/opt/data/etc}
CONF=rtunnel-agent.json
UNIT=rtunnel-agent.service

PKG_DIR=
VARIANT=
VERSION=
ACTION=install
START=0
UNITS=1
# 容器/临时前缀安装时可用环境变量彻底跳过 unit 操作
[ "${RTUNNEL_INSTALL_NO_UNITS:-0}" = "1" ] && UNITS=0

usage() {
    cat <<'EOF'
用法:
  install.sh --pkg-dir DIR --variant lite|web --version VER [--prefix DIR] [--start] [--no-units]
  install.sh --rollback [--prefix DIR]
  install.sh --status   [--prefix DIR]

--start     首次安装时 enable --now 服务；OTA 不要传
--no-units  只装二进制/配置，不碰 systemd（测试用）
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg-dir) PKG_DIR=$2; shift 2 ;;
        --variant) VARIANT=$2; shift 2 ;;
        --version) VERSION=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --start) START=1; shift ;;
        --no-units) UNITS=0; shift ;;
        --rollback) ACTION=rollback; shift ;;
        --status) ACTION=status; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
done

BIN=$PREFIX/bin
STATE=$PREFIX/state

# 只有标准安装路径强制 root；指定了别的 prefix 时按目标目录权限自然失败即可
if [ "$(id -u)" != 0 ] && [ "$PREFIX" = "/opt/rtunnel/agent" ]; then
    echo "需要 root 安装到 $PREFIX" >&2
    exit 3
fi

if [ "$ACTION" = status ]; then
    cur=$(readlink "$BIN/rtunnel-agent" 2>/dev/null || true)
    echo "prefix:   $PREFIX"
    echo "current:  ${cur:-未安装}"
    if [ -f "$STATE/previous" ]; then
        echo "previous: $(cut -f1 "$STATE/previous")"
    fi
    if [ -f "$STATE/pending" ]; then
        echo "pending:  $(cat "$STATE/pending")"
    fi
    exit 0
fi

if [ "$ACTION" = rollback ]; then
    [ -f "$STATE/previous" ] || { echo "没有可回滚的版本" >&2; exit 4; }
    prev=$(cut -f1 "$STATE/previous")
    [ -n "$prev" ] && [ -f "$BIN/$prev" ] || { echo "回滚目标不存在: $prev" >&2; exit 4; }
    tmp=$BIN/.rtunnel-agent.$$
    rm -f "$tmp"
    ln -s "$prev" "$tmp"
    mv -f "$tmp" "$BIN/rtunnel-agent"
    rm -f "$STATE/pending"
    echo "已回滚到 $prev；执行 systemctl restart $UNIT 生效"
    exit 0
fi

[ -n "$PKG_DIR" ] && [ -n "$VARIANT" ] && [ -n "$VERSION" ] || { usage >&2; exit 2; }
case "$VARIANT" in lite|web) ;; *) echo "variant 必须是 lite 或 web" >&2; exit 2 ;; esac

NEW_TARGET=rtunnel-agent-$VARIANT-$VERSION
SRC=$PKG_DIR/bin/$NEW_TARGET
[ -f "$SRC" ] || { echo "包内缺少 bin/$NEW_TARGET" >&2; exit 4; }
[ -f "$PKG_DIR/etc/$CONF" ] || { echo "包内缺少 etc/$CONF" >&2; exit 4; }
[ -f "$PKG_DIR/service/$UNIT" ] || { echo "包内缺少 service/$UNIT" >&2; exit 4; }

chmod 0755 "$SRC" 2>/dev/null || true
got=$("$SRC" --version 2>/dev/null | tr -d '\r\n' || true)
[ "$got" = "$VERSION" ] || { echo "版本自检失败: 期望 $VERSION，实际 ${got:-无法执行}" >&2; exit 4; }

if [ "$START" = 1 ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if ! grep -q -- '--devid' /etc/rtunnel/agent.env 2>/dev/null; then
        echo "请先在 /etc/rtunnel/agent.env 设置 AGENT_ARGS（至少包含 --devid 和 --server）" >&2
        exit 5
    fi
fi

mkdir -p "$BIN" "$STATE" "$PREFIX/service"

OLD_TARGET=$(readlink "$BIN/rtunnel-agent" 2>/dev/null || true)
OLD_TARGET=${OLD_TARGET##*/}

# 1. 版本化二进制：先写临时名再 rename，绝不覆盖正在执行的 inode
tmp=$BIN/.$NEW_TARGET.tmp.$$
cp "$SRC" "$tmp"
chmod 0755 "$tmp"
mv -f "$tmp" "$BIN/$NEW_TARGET"

# 2. 启动计数脚本；install.sh 自己也留一份，方便设备上 --status/--rollback
cp "$PKG_DIR/bin/agent-boot-check.sh" "$BIN/agent-boot-check.sh"
chmod 0755 "$BIN/agent-boot-check.sh"
cp "$PKG_DIR/install.sh" "$PREFIX/install.sh"
chmod 0755 "$PREFIX/install.sh"

# 3. 配置：不存在才装，已有配置不覆盖
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/$CONF" ]; then
    cp "$PKG_DIR/etc/$CONF" "$CONF_DIR/$CONF"
    echo "配置已安装: $CONF_DIR/$CONF"
fi
# 参考副本，便于以后对比新增键，不参与运行
cp "$PKG_DIR/etc/$CONF" "$CONF_DIR/$CONF.dist"

# 4. unit：只 enable/disable，不 stop/start/restart
if [ "$UNITS" = 1 ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    cp "$PKG_DIR/service/$UNIT" "$PREFIX/service/$UNIT"
    if ! cmp -s "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"; then
        cp "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
    systemctl enable "$UNIT" >/dev/null 2>&1 || true
fi

# 5. 状态：pending 先落盘，symlink 是事务提交点
if [ -n "$OLD_TARGET" ] && [ "$OLD_TARGET" != "$NEW_TARGET" ]; then
    printf '%s\t%s\n' "$OLD_TARGET" "$UNIT" > "$STATE/previous"
    printf '%s\t%s\t0\t0\n' "$OLD_TARGET" "$NEW_TARGET" > "$STATE/pending"
else
    rm -f "$STATE/previous" "$STATE/pending"
fi
printf '%s\n' "$UNIT" > "$STATE/unit"

# 6. 原子切换软链（mv 覆盖 symlink 走 rename，不存在空窗期）
tmp=$BIN/.rtunnel-agent.$$
rm -f "$tmp"
ln -s "$NEW_TARGET" "$tmp"
mv -f "$tmp" "$BIN/rtunnel-agent"

# 7. 清理：保留当前、上一个和最近 3 个
files=$(ls -1t "$BIN"/rtunnel-agent-*-* 2>/dev/null || true)
i=0
for f in $files; do
    i=$((i + 1))
    if [ "$i" -le 3 ]; then continue; fi
    b=${f##*/}
    if [ "$b" = "$NEW_TARGET" ]; then continue; fi
    if [ -n "$OLD_TARGET" ] && [ "$b" = "$OLD_TARGET" ]; then continue; fi
    rm -f "$f"
done

echo "INSTALLED=$BIN/$NEW_TARGET"

if [ "$START" = 1 ]; then
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl enable --now "$UNIT"
    else
        echo "非 systemd 环境，请手动启动: $BIN/rtunnel-agent"
    fi
fi
