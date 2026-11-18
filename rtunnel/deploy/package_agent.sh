#!/bin/bash
# 生成 OTA/手工部署包：
#   rtunnel-agent-lite-<ver>-<arch>.tar.gz
#   rtunnel-agent-web-<ver>-<arch>.tar.gz
#
# 包内结构（解压出一层同名目录）：
#   manifest.json
#   install.sh
#   bin/rtunnel-agent-<variant>-<ver>
#   bin/agent-boot-check.sh
#   etc/rtunnel-agent.json
#   service/rtunnel-agent.service
#
# 版本默认取部署二进制的 --version，必须与文件名一致（设备侧会自检）。
# 架构默认取 uname -m；交叉编译时用 RTUNNEL_ARCH 指定目标架构。
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
SRC_DIR="$ROOT/rtunnel/deploy/agent"
LITE="$DEPLOY/agent/bin/rtunnel-agent"
WEB="$DEPLOY/agent-web/bin/rtunnel-agent"
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"

[ -x "$LITE" ] || { echo "缺少 $LITE，先构建 rtunnel-deploy" >&2; exit 1; }
case "$ARCH" in ''|*/*|*' '*) echo "架构不合法: $ARCH" >&2; exit 1 ;; esac

# 交叉编译时用 qemu 跑目标二进制做版本自检，例如：
#   RTUNNEL_PKG_RUNNER="qemu-arm-static -L /opt/st/.../sysroots/cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi"
RUN=${RTUNNEL_PKG_RUNNER:-}
run_bin() {
    if [ -n "$RUN" ]; then
        $RUN "$1" "${@:2}"
    else
        "$1" "${@:2}"
    fi
}

VER="${1:-$(run_bin "$LITE" --version 2>/dev/null | tr -d '\r\n')}"
[ -n "$VER" ] || { echo "无法获取版本，请显式传入：$0 1.4.9" >&2; exit 1; }
case "$VER" in */*|*' '*) echo "版本号不合法: $VER" >&2; exit 1 ;; esac

if [ -x "$WEB" ]; then
    webver=$(run_bin "$WEB" --version 2>/dev/null | tr -d '\r\n')
    [ "$webver" = "$VER" ] || { echo "lite/web 版本不一致: $VER vs ${webver:-?}" >&2; exit 1; }
fi

mkdir -p "$DEPLOY/packages"
STAGE="$(mktemp -d "$DEPLOY/.pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

make_one() {
    local variant="$1" src="$2"
    local binname="rtunnel-agent-$variant-$VER"
    local name="$binname-$ARCH"
    local d="$STAGE/$name"
    mkdir -p "$d/bin" "$d/etc" "$d/service"
    cp "$src" "$d/bin/$binname"
    chmod 0755 "$d/bin/$binname"
    cp "$SRC_DIR/agent-boot-check.sh" "$d/bin/agent-boot-check.sh"
    chmod 0755 "$d/bin/agent-boot-check.sh"
    cp "$SRC_DIR/install.sh" "$d/install.sh"
    chmod 0755 "$d/install.sh"
    cp "$SRC_DIR/rtunnel-agent.json" "$d/etc/rtunnel-agent.json"
    cp "$SRC_DIR/rtunnel-agent.service" "$d/service/rtunnel-agent.service"

    printf '{"name":"rtunnel-agent","variant":"%s","version":"%s","arch":"%s","binary":"bin/%s"}\n' \
        "$variant" "$VER" "$ARCH" "$binname" > "$d/manifest.json"

    # --format=ustar：设备侧解析器只认 ustar，长文件名用 prefix 字段
    tar --format=ustar -C "$STAGE" -cf - "$name" | gzip -9 -n > "$DEPLOY/packages/$name.tar.gz"
    echo "  $DEPLOY/packages/$name.tar.gz"
}

echo "打包版本 $VER ($ARCH)"
make_one lite "$LITE"
if [ -x "$WEB" ]; then
    make_one web "$WEB"
else
    echo "警告: 未找到 $WEB，只生成 lite 包" >&2
fi

ls -lh "$DEPLOY"/packages/rtunnel-agent-*.tar.gz
