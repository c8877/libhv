#!/bin/sh
# rtunnel-client 安装/升级脚本（部署包内唯一入口）。
#
# 客户端没有必须常驻的服务：装完只是把二进制放到 /opt/rtunnel/client，
# 并在 /usr/local/bin 下建软链。需要常驻 SOCKS 时再装可选的 unit。
#
#   install.sh --pkg-dir DIR [--prefix P] [--bin-dir D] [--start|--restart] [--no-units]
#   install.sh --rollback [--prefix P]
#   install.sh --status   [--prefix P]
#
# 退出码: 0 成功, 2 参数错误, 3 权限不足, 4 无可回滚版本/缺文件,
#         5 包校验失败, 6 版本降级且未加 --force
set -eu

PREFIX=${RTUNNEL_CLIENT_PREFIX:-/opt/rtunnel/client}
BIN_DIR=${RTUNNEL_CLIENT_BIN_DIR:-/usr/local/bin}
CONF_DIR=${RTUNNEL_CONF_DIR:-}
CONF=client.env
UNIT=rtunnel-client-socks.service

PKG_DIR=
ACTION=install
START=0
RESTART=0
UNITS=1
FORCE=0
[ "${RTUNNEL_INSTALL_NO_UNITS:-0}" = "1" ] && UNITS=0

usage() {
    cat <<'EOF'
用法:
  install.sh --pkg-dir DIR [--prefix P] [--bin-dir D] [--start|--restart] [--no-units] [--force]
  install.sh --rollback [--prefix P]
  install.sh --status   [--prefix P]

--start/--restart  安装后启用并启动可选 SOCKS 服务（未装 unit 时忽略）
--no-units         只装二进制/配置，不碰 systemd（测试/容器用）
--bin-dir          软链目录，默认 /usr/local/bin（无写权限时自动跳过）
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg-dir) PKG_DIR=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --bin-dir) BIN_DIR=$2; shift 2 ;;
        --start) START=1; shift ;;
        --restart) RESTART=1; shift ;;
        --no-units) UNITS=0; shift ;;
        --force) FORCE=1; shift ;;
        --rollback) ACTION=rollback; shift ;;
        --status) ACTION=status; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
done

BIN=$PREFIX/bin
STATE=$PREFIX/state

# 临时前缀（测试/容器）默认把配置放在 prefix 下
if [ "$PREFIX" = "/opt/rtunnel/client" ]; then
    [ -n "$CONF_DIR" ] || CONF_DIR=/etc/rtunnel
else
    [ -n "$CONF_DIR" ] || CONF_DIR=$PREFIX/etc
fi

if [ "$(id -u)" != 0 ] && [ "$PREFIX" = "/opt/rtunnel/client" ]; then
    echo "需要 root 安装到 $PREFIX" >&2
    exit 3
fi

if [ "$ACTION" = status ]; then
    cur=$(readlink "$BIN/rtunnel-client" 2>/dev/null || true)
    echo "prefix:   $PREFIX"
    echo "current:  ${cur:-未安装}"
    if [ -f "$STATE/previous" ]; then
        echo "previous: $(cut -f1 "$STATE/previous")"
    fi
    if [ -L "$BIN_DIR/rtunnel-client" ]; then
        echo "link:     $BIN_DIR/rtunnel-client -> $(readlink "$BIN_DIR/rtunnel-client")"
    fi
    exit 0
fi

do_rollback() {
    prev=$(cut -f1 "$STATE/previous" 2>/dev/null || true)
    if [ -z "$prev" ] || [ ! -f "$BIN/$prev" ]; then
        echo "回滚目标不存在: ${prev:-无}" >&2
        return 4
    fi
    tmp=$BIN/.rtunnel-client.$$
    rm -f "$tmp"
    ln -s "$prev" "$tmp"
    mv -f "$tmp" "$BIN/rtunnel-client"
    echo "已切换到 $prev"
    return 0
}

if [ "$ACTION" = rollback ]; then
    do_rollback || exit 4
    exit 0
fi

[ -n "$PKG_DIR" ] || { usage >&2; exit 2; }
[ -f "$PKG_DIR/manifest.json" ] || { echo "包内缺少 manifest.json" >&2; exit 4; }
[ -f "$PKG_DIR/bin/rtunnel-client" ] || { echo "包内缺少 bin/rtunnel-client" >&2; exit 4; }
[ -f "$PKG_DIR/install.sh" ] || { echo "包内缺少 install.sh" >&2; exit 4; }

json_str() {
    sed -n 's/.*"'"$1"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$PKG_DIR/manifest.json" | head -1
}
M_NAME=$(json_str name)
M_VERSION=$(json_str version)
M_ARCH=$(json_str arch)
[ "$M_NAME" = "rtunnel-client" ] || { echo "manifest name 不是 rtunnel-client: $M_NAME" >&2; exit 5; }
[ -n "$M_VERSION" ] && [ -n "$M_ARCH" ] || { echo "manifest 缺少 version/arch" >&2; exit 5; }

HOST_ARCH=$(uname -m)
if [ "${RTUNNEL_IGNORE_ARCH:-0}" != 1 ] && [ "$M_ARCH" != "$HOST_ARCH" ]; then
    echo "架构不匹配: 包 $M_ARCH，本机 $HOST_ARCH" >&2
    exit 5
fi

if [ -f "$PKG_DIR/SHA256SUMS" ]; then
    ( cd "$PKG_DIR" && sha256sum -c SHA256SUMS >/dev/null ) || {
        echo "包内文件 sha256 校验失败" >&2
        exit 5
    }
fi

chmod 0755 "$PKG_DIR/bin/rtunnel-client"
got=$("$PKG_DIR/bin/rtunnel-client" --version 2>/dev/null | tr -d '\r\n' || true)
[ "$got" = "$M_VERSION" ] || { echo "版本自检失败: 期望 $M_VERSION，实际 ${got:-无法执行}" >&2; exit 5; }

CUR_TARGET=$(readlink "$BIN/rtunnel-client" 2>/dev/null || true)
CUR_TARGET=${CUR_TARGET##*/}
CUR_VER=${CUR_TARGET#rtunnel-client-}
if [ "$FORCE" != 1 ] && [ -n "$CUR_VER" ] && [ "$CUR_VER" != "$M_VERSION" ]; then
    lower=$(printf '%s\n%s\n' "$CUR_VER" "$M_VERSION" | sort -V | head -1)
    if [ "$lower" = "$M_VERSION" ]; then
        echo "拒绝降级: 当前 $CUR_VER -> 目标 $M_VERSION（加 --force 可覆盖）" >&2
        exit 6
    fi
fi

mkdir -p "$BIN" "$STATE" "$PREFIX/service" "$CONF_DIR"

NEW_TARGET=rtunnel-client-$M_VERSION
tmp=$BIN/.$NEW_TARGET.tmp.$$
cp "$PKG_DIR/bin/rtunnel-client" "$tmp"
chmod 0755 "$tmp"
mv -f "$tmp" "$BIN/$NEW_TARGET"

cp "$PKG_DIR/install.sh" "$PREFIX/install.sh"
chmod 0755 "$PREFIX/install.sh"
# 自举脚本副本：设备上可继续用同一个脚本升级其它组件
if [ -f "$PKG_DIR/bin/rt-bootstrap.sh" ]; then
    cp "$PKG_DIR/bin/rt-bootstrap.sh" "$PREFIX/rt-bootstrap.sh"
    chmod 0755 "$PREFIX/rt-bootstrap.sh"
fi

if [ ! -f "$CONF_DIR/$CONF" ]; then
    if [ -f "$PKG_DIR/etc/client.args.example" ]; then
        cp "$PKG_DIR/etc/client.args.example" "$CONF_DIR/$CONF"
    else
        printf 'RT="https://127.0.0.1:9443"\nATK_FILE="/etc/rtunnel/atk"\n' > "$CONF_DIR/$CONF"
    fi
    chmod 0600 "$CONF_DIR/$CONF"
    echo "配置已安装: $CONF_DIR/$CONF"
fi

# 可选 SOCKS unit：包内带了才安装；--start/--restart 时启用
if [ "$UNITS" = 1 ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if [ -f "$PKG_DIR/service/$UNIT" ]; then
        sed -e "s|@PREFIX@|$PREFIX|g" "$PKG_DIR/service/$UNIT" > "$PREFIX/service/$UNIT"
        if ! cmp -s "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"; then
            cp "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"
            systemctl daemon-reload >/dev/null 2>&1 || true
        fi
        if [ "$START" = 1 ] || [ "$RESTART" = 1 ]; then
            systemctl enable "$UNIT" >/dev/null 2>&1 || true
            systemctl restart "$UNIT" || true
        fi
    fi
fi

if [ -n "$CUR_TARGET" ] && [ "$CUR_TARGET" != "$NEW_TARGET" ]; then
    printf '%s\t%s\n' "$CUR_TARGET" "$UNIT" > "$STATE/previous"
else
    rm -f "$STATE/previous"
fi

tmp=$BIN/.rtunnel-client.$$
rm -f "$tmp"
ln -s "$NEW_TARGET" "$tmp"
mv -f "$tmp" "$BIN/rtunnel-client"

# /usr/local/bin 软链：目录不可写时只提示，不影响安装
if [ -n "$BIN_DIR" ] && [ -d "$BIN_DIR" ] && [ -w "$BIN_DIR" ]; then
    tmp=$BIN_DIR/.rtunnel-client.$$
    rm -f "$tmp"
    ln -s "$BIN/rtunnel-client" "$tmp" 2>/dev/null && mv -f "$tmp" "$BIN_DIR/rtunnel-client" || rm -f "$tmp"
fi

keep1=$NEW_TARGET
keep2=$(cut -f1 "$STATE/previous" 2>/dev/null || true)
for f in "$BIN"/rtunnel-client-*; do
    [ -f "$f" ] || continue
    b=${f##*/}
    [ "$b" = "$keep1" ] && continue
    [ -n "$keep2" ] && [ "$b" = "$keep2" ] && continue
    case "$b" in .*) continue ;; esac
    rm -f "$f"
done

echo "INSTALLED=$BIN/$NEW_TARGET"
