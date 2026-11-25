#!/bin/bash
# 构建 relay / client 部署包（agent 包由 package_agent.sh 生成）。
#
# 前置：已执行 rtunnel-deploy（或至少产出 build/deploy/{relay,agent}/bin）。
# 产物：<out>/rtunnel-<component>-<ver>-<arch>.tar.gz 与 .sha256
#
# 用法：
#   ./deploy/build_packages.sh [VERSION] [--arch ARCH] [--out DIR] [--component all|relay|client]
#
# 环境变量：
#   RTUNNEL_DEPLOY_DIR  构建产物目录，默认 build/deploy
#   RTUNNEL_ARCH        目标架构，默认 uname -m
#   RTUNNEL_PKG_RUNNER  交叉编译时用 qemu 跑目标二进制做 --version 自检
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
OUT=""
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"
COMPONENT=all
VER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --component) COMPONENT=$2; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "未知参数: $1" >&2; exit 2 ;;
        *) VER=$1; shift ;;
    esac
done

OUT="${OUT:-$DEPLOY/packages}"
case "$ARCH" in ''|*/*|*' '*) echo "架构不合法: $ARCH" >&2; exit 1 ;; esac
case "$COMPONENT" in all|relay|client) ;; *) echo "component 只能是 all/relay/client（agent 用 package_agent.sh）" >&2; exit 2 ;; esac

RUN=${RTUNNEL_PKG_RUNNER:-}
run_bin() {
    if [ -n "$RUN" ]; then $RUN "$1" "${@:2}"; else "$1" "${@:2}"; fi
}
ver_of() {
    run_bin "$1" --version 2>/dev/null | tr -d '\r\n'
}

RELAY_BIN="$DEPLOY/relay/bin/rtunnel-relay"
CLIENT_BIN="$DEPLOY/agent/bin/rtunnel-client"

if [ "$COMPONENT" = all ] || [ "$COMPONENT" = relay ]; then
    [ -x "$RELAY_BIN" ] || { echo "缺少 $RELAY_BIN，先构建 rtunnel-deploy（含 relay 变体）" >&2; exit 1; }
fi
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = client ]; then
    [ -x "$CLIENT_BIN" ] || { echo "缺少 $CLIENT_BIN，先构建 rtunnel-deploy" >&2; exit 1; }
fi

if [ -z "$VER" ]; then
    if [ -x "$RELAY_BIN" ]; then VER="$(ver_of "$RELAY_BIN")"; elif [ -x "$CLIENT_BIN" ]; then VER="$(ver_of "$CLIENT_BIN")"; fi
fi
[ -n "$VER" ] || { echo "无法获取版本，请显式传入：$0 1.4.130" >&2; exit 1; }
case "$VER" in */*|*' '*) echo "版本号不合法: $VER" >&2; exit 1 ;; esac

mkdir -p "$OUT"
STAGE="$(mktemp -d "$DEPLOY/.pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

finish() {
    local dir="$1" file="$2"
    ( cd "$dir/$file" && find . -type f ! -name SHA256SUMS -printf '%P\n' | sort | xargs -r sha256sum > SHA256SUMS )
    tar --format=ustar -C "$dir" -cf - "$file" | gzip -9 -n > "$OUT/$file.tar.gz"
    ( cd "$OUT" && sha256sum "$file.tar.gz" > "$file.tar.gz.sha256" )
    echo "  $OUT/$file.tar.gz"
}

# ---------------------------------------------------------------- relay
build_relay() {
    local name="rtunnel-relay-$VER-$ARCH"
    local d="$STAGE/$name"
    local bin="$RELAY_BIN"
    # 内嵌前端时部署目录里没有 web/；否则带上磁盘前端
    local webdir="$DEPLOY/relay/web/dist"
    mkdir -p "$d/bin" "$d/etc" "$d/service"
    cp "$bin" "$d/bin/rtunnel-relay"
    chmod 0755 "$d/bin/rtunnel-relay"
    if [ -x "$CLIENT_BIN" ]; then
        cp "$CLIENT_BIN" "$d/bin/rtunnel-client"
        chmod 0755 "$d/bin/rtunnel-client"
    fi
    cp "$ROOT/rtunnel/deploy/relay/install.sh" "$d/install.sh"
    chmod 0755 "$d/install.sh"
    cp "$ROOT/rtunnel/deploy/rt-bootstrap.sh" "$d/bin/rt-bootstrap.sh"
    chmod 0755 "$d/bin/rt-bootstrap.sh"
    cp "$ROOT/rtunnel/deploy/relay/relay.args.example" "$d/etc/relay.args.example"
    cp "$ROOT/rtunnel/deploy/relay/rtunnel-relay.service" "$d/service/rtunnel-relay.service"
    if [ -d "$webdir" ] && [ -n "$(ls -A "$webdir" 2>/dev/null)" ]; then
        mkdir -p "$d/web/dist"
        cp -a "$webdir/." "$d/web/dist/"
    fi

    local got; got=$(ver_of "$d/bin/rtunnel-relay")
    [ "$got" = "$VER" ] || { echo "relay 版本自检失败: $VER vs ${got:-?}" >&2; exit 1; }

    printf '{"name":"rtunnel-relay","version":"%s","arch":"%s","binary":"bin/rtunnel-relay"}\n' \
        "$VER" "$ARCH" > "$d/manifest.json"
    finish "$STAGE" "$name"
}

# ---------------------------------------------------------------- client
build_client() {
    local name="rtunnel-client-$VER-$ARCH"
    local d="$STAGE/$name"
    mkdir -p "$d/bin" "$d/etc" "$d/service"
    cp "$CLIENT_BIN" "$d/bin/rtunnel-client"
    chmod 0755 "$d/bin/rtunnel-client"
    cp "$ROOT/rtunnel/deploy/client/install.sh" "$d/install.sh"
    chmod 0755 "$d/install.sh"
    cp "$ROOT/rtunnel/deploy/rt-bootstrap.sh" "$d/bin/rt-bootstrap.sh"
    chmod 0755 "$d/bin/rt-bootstrap.sh"
    cp "$ROOT/rtunnel/deploy/client/client.args.example" "$d/etc/client.args.example"
    cp "$ROOT/rtunnel/deploy/client/rtunnel-client-socks.service" "$d/service/rtunnel-client-socks.service"

    local got; got=$(ver_of "$d/bin/rtunnel-client")
    [ "$got" = "$VER" ] || { echo "client 版本自检失败: $VER vs ${got:-?}" >&2; exit 1; }

    printf '{"name":"rtunnel-client","version":"%s","arch":"%s","binary":"bin/rtunnel-client"}\n' \
        "$VER" "$ARCH" > "$d/manifest.json"
    finish "$STAGE" "$name"
}

echo "打包版本 $VER ($ARCH) -> $OUT"
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = relay ]; then
    build_relay
fi
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = client ]; then
    build_client
fi
ls -lh "$OUT"/rtunnel-{relay,client}-*.tar.gz 2>/dev/null || true
