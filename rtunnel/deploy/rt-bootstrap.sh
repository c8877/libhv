#!/bin/bash
# rt-bootstrap.sh —— 在目标机上用 rtunnel-client 完成“下载 → 校验 → 解包 → 安装”。
#
# 前置：机器上已有一个可用的 rtunnel-client（首次用 scp/镜像预置），并且有
# 可读的 atk- 令牌文件（只需 repo.read；发布机才有 repo.write）。
#
#   rt-bootstrap.sh --component relay|agent|client [--version latest|1.4.130]
#                   [--arch $(uname -m)] [--variant web|lite]
#                   [--rt URL] [--atk-file F] [--insecure] [--tls-ca F]
#                   [--client BIN] [--workdir DIR] [--prefix P]
#                   [--start|--restart] [--boot] [--keep]
#   rt-bootstrap.sh --component C --status
#   rt-bootstrap.sh --component C --rollback [--restart]
#
# 退出码: 0 成功, 2 参数错误, 3 缺少 client/令牌, 4 下载或解包失败,
#         5 校验失败, 其余透传 install.sh 的退出码。
set -eu

COMPONENT=
VERSION=latest
ARCH=$(uname -m)
VARIANT=web
RT="${RT:-}"
ATK_FILE="${ATK_FILE:-}"
TLS_CA=""
INSECURE=0
CLIENT="${RTUNNEL_CLIENT:-}"
WORKDIR="${RTUNNEL_BOOTSTRAP_WORKDIR:-/tmp/rtunnel-bootstrap}"
PREFIX=""
ACTION=install
START=0
RESTART=0
BOOT=0
KEEP=0
NO_UNITS=0

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --component) COMPONENT=$2; shift 2 ;;
        --version) VERSION=$2; shift 2 ;;
        --arch) ARCH=$2; shift 2 ;;
        --variant) VARIANT=$2; shift 2 ;;
        --rt) RT=$2; shift 2 ;;
        --atk-file) ATK_FILE=$2; shift 2 ;;
        --tls-ca) TLS_CA=$2; shift 2 ;;
        --insecure) INSECURE=1; shift ;;
        --client) CLIENT=$2; shift 2 ;;
        --workdir) WORKDIR=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --start) START=1; shift ;;
        --restart) RESTART=1; shift ;;
        --boot) BOOT=1; shift ;;
        --keep) KEEP=1; shift ;;
        --no-units) NO_UNITS=1; shift ;;
        --status) ACTION=status; shift ;;
        --rollback) ACTION=rollback; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$COMPONENT" in relay|agent|client) ;; *) echo "--component 必须是 relay/agent/client" >&2; exit 2 ;; esac
case "$VARIANT" in lite|web) ;; *) echo "--variant 只能是 lite/web" >&2; exit 2 ;; esac

if [ -z "$PREFIX" ]; then
    case "$COMPONENT" in
        relay) PREFIX=/opt/rtunnel/relay ;;
        agent) PREFIX=/opt/rtunnel/agent ;;
        client) PREFIX=/opt/rtunnel/client ;;
    esac
fi

# ---- 运维子命令：交给已安装的 install.sh
if [ "$ACTION" = status ] || [ "$ACTION" = rollback ]; then
    if [ ! -x "$PREFIX/install.sh" ]; then
        echo "未安装（$PREFIX/install.sh 不存在），没有可查询/回滚的版本" >&2
        exit 4
    fi
    if [ "$ACTION" = status ]; then
        exec "$PREFIX/install.sh" --status --prefix "$PREFIX"
    fi
    set -- --rollback --prefix "$PREFIX"
    [ "$RESTART" = 1 ] && set -- "$@" --restart
    exec "$PREFIX/install.sh" "$@"
fi

# ---- 定位 client
if [ -z "$CLIENT" ]; then
    if command -v rtunnel-client >/dev/null 2>&1; then
        CLIENT=rtunnel-client
    else
        echo "找不到 rtunnel-client：用 --client 指定，或先预置到 PATH" >&2
        exit 3
    fi
fi

REPO=(--rt "$RT")
[ -n "$RT" ] || { echo "缺少 --rt（或环境变量 RT）" >&2; exit 2; }
[ -n "$ATK_FILE" ] || { echo "缺少 --atk-file（或环境变量 ATK_FILE）" >&2; exit 2; }
REPO+=(--atk-file "$ATK_FILE")
[ "$INSECURE" = 1 ] && REPO+=(--insecure)
[ -n "$TLS_CA" ] && REPO+=(--tls-ca "$TLS_CA")

repo() { "$CLIENT" repo "$@" "${REPO[@]}"; }

json_get() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1
}

mkdir -p "$WORKDIR"
TMP=$(mktemp -d "$WORKDIR/.run.XXXXXX")
cleanup() {
    [ "$KEEP" = 1 ] || rm -rf "$TMP"
}
trap cleanup EXIT

# ---- 解析 latest（文件指针与包同仓库，sha256 由发布机写入）
COMP=$COMPONENT
if [ "$VERSION" = latest ]; then
    case "$COMP" in
        agent) LATEST="packages/agent/latest/$ARCH-$VARIANT.json" ;;
        *) LATEST="packages/$COMP/latest/$ARCH.json" ;;
    esac
    echo "读取 $LATEST"
    repo get --dir "$(dirname "$LATEST")" --name "$(basename "$LATEST")" --out "$TMP/latest.json" --quiet
    VERSION=$(json_get "$TMP/latest.json" version)
    FILE=$(json_get "$TMP/latest.json" file)
    WANT_SHA=$(json_get "$TMP/latest.json" sha256)
    [ -n "$VERSION" ] && [ -n "$FILE" ] || { echo "latest 指针缺字段" >&2; exit 4; }
else
    case "$COMP" in
        relay) FILE="rtunnel-relay-$VERSION-$ARCH.tar.gz" ;;
        client) FILE="rtunnel-client-$VERSION-$ARCH.tar.gz" ;;
        agent) FILE="rtunnel-agent-$VARIANT-$VERSION-$ARCH.tar.gz" ;;
    esac
    WANT_SHA=""
fi

echo "组件 $COMP 版本 $VERSION 架构 $ARCH"

# ---- 下载
PKG_DIR="packages/$COMP/$VERSION/$ARCH"
repo get --dir "$PKG_DIR" --name "$FILE" --out "$TMP/$FILE" --resume --quiet

# ---- 校验：优先与 latest 指针比对，其次与随包 .sha256 比对
got=$(sha256sum "$TMP/$FILE" | awk '{print $1}')
if [ -n "$WANT_SHA" ]; then
    [ "$got" = "$WANT_SHA" ] || { echo "sha256 不匹配（latest 指针）: $got vs $WANT_SHA" >&2; exit 5; }
else
    if repo get --dir "$PKG_DIR" --name "$FILE.sha256" --out "$TMP/$FILE.sha256" --quiet 2>/dev/null; then
        want=$(awk '{print $1}' "$TMP/$FILE.sha256")
        [ "$got" = "$want" ] || { echo "sha256 不匹配（.sha256）: $got vs $want" >&2; exit 5; }
    else
        echo "警告: 没有 $FILE.sha256，只有传输层校验" >&2
    fi
fi

# ---- 解包
EXDIR="$TMP/ex"
mkdir -p "$EXDIR"
tar -xzf "$TMP/$FILE" -C "$EXDIR" || { echo "解包失败: $FILE" >&2; exit 4; }
PKGROOT="$EXDIR/rtunnel-$COMP-$VERSION-$ARCH"
if [ ! -f "$PKGROOT/manifest.json" ]; then
    # 兼容目录名与预期不同：找第一个带 manifest.json 的一级目录
    PKGROOT=
    for d in "$EXDIR"/*; do
        [ -d "$d" ] && [ -f "$d/manifest.json" ] && PKGROOT=$d && break
    done
fi
[ -n "$PKGROOT" ] && [ -f "$PKGROOT/manifest.json" ] || { echo "包内没有 manifest.json" >&2; exit 4; }

# ---- 安装
set -- --pkg-dir "$PKGROOT" --prefix "$PREFIX"
case "$COMP" in
    agent) set -- "$@" --variant "$VARIANT" --version "$VERSION" ;;
esac
[ "$START" = 1 ] && set -- "$@" --start
[ "$RESTART" = 1 ] && set -- "$@" --restart
[ "$BOOT" = 1 ] && set -- "$@" --boot
[ "$NO_UNITS" = 1 ] && set -- "$@" --no-units
echo "执行 $PKGROOT/install.sh $*"
sh "$PKGROOT/install.sh" "$@"
