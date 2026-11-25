#!/bin/bash
# 生成 OTA/手工部署包（agent）：
#   rtunnel-agent-lite-<ver>-<arch>.tar.gz
#   rtunnel-agent-web-<ver>-<arch>.tar.gz
#
# 包内结构（解压出一层 rtunnel-agent-<ver>-<arch>/）：
#   manifest.json          name/variant/version/arch/binary
#   SHA256SUMS             包内文件校验清单（install.sh 会核对）
#   install.sh
#   bin/rtunnel-agent      与 relay/client 同构的统一入口名
#   bin/agent-boot-check.sh
#   bin/rtunnel-client     设备侧自举/排障用（构建了才带）
#   etc/rtunnel-agent.json
#   service/rtunnel-agent.service
#
# 文件名保留 <lite|web> 以兼容 OTA 页面的选择逻辑；解压目录不带 variant。
# 版本默认取部署二进制的 --version，必须与 manifest 一致（设备侧会自检）。
# 架构默认取 uname -m；交叉编译时用 RTUNNEL_ARCH 指定目标架构。
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
SRC_DIR="$ROOT/rtunnel/deploy/agent"
LITE="$DEPLOY/agent/bin/rtunnel-agent"
WEB="$DEPLOY/agent-web/bin/rtunnel-agent"
CLIENT="$DEPLOY/agent/bin/rtunnel-client"
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

HAVE_WEB=0
if [ -x "$WEB" ]; then
    webver=$(run_bin "$WEB" --version 2>/dev/null | tr -d '\r\n' || true)
    if [ "$webver" = "$VER" ]; then
        HAVE_WEB=1
    elif [ "${RTUNNEL_REQUIRE_WEB:-0}" = 1 ]; then
        echo "web 版本不一致: $VER vs ${webver:-无法执行}" >&2
        exit 1
    else
        echo "警告: $WEB 版本为 ${webver:-无法执行}，与 $VER 不符，跳过 web 包（交叉编译请设 RTUNNEL_PKG_RUNNER）" >&2
    fi
fi

mkdir -p "$DEPLOY/packages"
STAGE="$(mktemp -d "$DEPLOY/.pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

make_one() {
    local variant="$1" src="$2"
    local name="rtunnel-agent-$VER-$ARCH"           # 解压后的目录
    local file="rtunnel-agent-$variant-$VER-$ARCH"  # tar.gz 基名（OTA 按它选变体）
    local d="$STAGE/$name"
    rm -rf "$d"
    mkdir -p "$d/bin" "$d/etc" "$d/service"
    cp "$src" "$d/bin/rtunnel-agent"
    chmod 0755 "$d/bin/rtunnel-agent"
    cp "$SRC_DIR/agent-boot-check.sh" "$d/bin/agent-boot-check.sh"
    chmod 0755 "$d/bin/agent-boot-check.sh"
    if [ -x "$CLIENT" ]; then
        cp "$CLIENT" "$d/bin/rtunnel-client"
        chmod 0755 "$d/bin/rtunnel-client"
    fi
    cp "$SRC_DIR/install.sh" "$d/install.sh"
    chmod 0755 "$d/install.sh"
    cp "$ROOT/rtunnel/deploy/rt-bootstrap.sh" "$d/bin/rt-bootstrap.sh"
    chmod 0755 "$d/bin/rt-bootstrap.sh"
    cp "$SRC_DIR/rtunnel-agent.json" "$d/etc/rtunnel-agent.json"
    cp "$SRC_DIR/rtunnel-agent.service" "$d/service/rtunnel-agent.service"

    # 解压目录与包内入口统一为 bin/rtunnel-agent；variant 只体现在 tar 文件名
    printf '{"name":"rtunnel-agent","variant":"%s","version":"%s","arch":"%s","binary":"bin/rtunnel-agent"}\n' \
        "$variant" "$VER" "$ARCH" > "$d/manifest.json"

    # 包内校验清单（install.sh 会执行 sha256sum -c）
    ( cd "$d" && find . -type f ! -name SHA256SUMS -printf '%P\n' | sort | xargs -r sha256sum > SHA256SUMS )

    # --format=ustar：设备侧解析器只认 ustar，长文件名用 prefix 字段
    tar --format=ustar -C "$STAGE" -cf - "$name" | gzip -9 -n > "$DEPLOY/packages/$file.tar.gz"
    ( cd "$DEPLOY/packages" && sha256sum "$file.tar.gz" > "$file.tar.gz.sha256" )
    echo "  $DEPLOY/packages/$file.tar.gz"
}

echo "打包版本 $VER ($ARCH)"
make_one lite "$LITE"
if [ "$HAVE_WEB" = 1 ]; then
    make_one web "$WEB"
else
    echo "只生成 lite 包（web 变体未构建或版本不符）" >&2
fi

ls -lh "$DEPLOY"/packages/rtunnel-agent-*.tar.gz
