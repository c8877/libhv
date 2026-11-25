#!/bin/bash
# 把 build_packages.sh / package_agent.sh 生成的部署包发布到 relay 文件仓库。
#
# 仓库布局：
#   packages/<component>/<version>/<arch>/<pkg>.tar.gz(+.sha256)
#   packages/<component>/latest/<arch>.json                 （relay/client）
#   packages/<component>/latest/<arch>-<variant>.json       （agent）
#
# 幂等：同 sha256 的包重复发布会跳过（rtunnel-client repo put 的默认行为）。
#
# 用法：
#   ./deploy/publish.sh --version 1.4.130 --arch x86_64 [--component all|relay|agent|client]
#                       [--variant all|lite|web] [--dir build/deploy/packages]
#                       [--rt https://host:9443] [--atk-file F] [--insecure] [--client BIN]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VER=""
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"
COMPONENT=all
VARIANT=all
PKG_DIR="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}/packages"
RT="${RT:-}"
ATK_FILE="${ATK_FILE:-}"
INSECURE=0
CLIENT="${RTUNNEL_CLIENT:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VER=$2; shift 2 ;;
        --arch) ARCH=$2; shift 2 ;;
        --component) COMPONENT=$2; shift 2 ;;
        --variant) VARIANT=$2; shift 2 ;;
        --dir) PKG_DIR=$2; shift 2 ;;
        --rt) RT=$2; shift 2 ;;
        --atk-file) ATK_FILE=$2; shift 2 ;;
        --insecure) INSECURE=1; shift ;;
        --client) CLIENT=$2; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

case "$COMPONENT" in all|relay|agent|client) ;; *) echo "component 只能是 all/relay/agent/client" >&2; exit 2 ;; esac
case "$VARIANT" in all|lite|web) ;; *) echo "variant 只能是 all/lite/web" >&2; exit 2 ;; esac
[ -n "$VER" ] || { echo "缺少 --version" >&2; exit 2; }
[ -n "$RT" ] || { echo "缺少 --rt（或环境变量 RT）" >&2; exit 2; }
[ -n "$ATK_FILE" ] || { echo "缺少 --atk-file（或环境变量 ATK_FILE）" >&2; exit 2; }

# 优先用已安装/指定 client，回退到构建目录
if [ -z "$CLIENT" ]; then
    if [ -x "$ROOT/build/deploy/agent/bin/rtunnel-client" ]; then
        CLIENT="$ROOT/build/deploy/agent/bin/rtunnel-client"
    else
        CLIENT=rtunnel-client
    fi
fi

COMMON=(--rt "$RT" --atk-file "$ATK_FILE")
[ "$INSECURE" = 1 ] && COMMON+=(--insecure)

repo() { "$CLIENT" repo "$@" "${COMMON[@]}"; }

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}

# 上传一个文件并输出 sha256
put_one() {
    local dir="$1" file="$2"
    repo put --dir "$dir" --file "$file" --quiet
}

# 生成并上传 latest 指针；最后上传，避免指向半截文件
put_latest() {
    local comp="$1" name="$2" file="$3" extra="$4"
    local tmp; tmp=$(mktemp)
    local sha; sha=$(sha256_of "$PKG_DIR/$file")
    printf '{"component":"%s","version":"%s","arch":"%s"%s,"file":"%s","sha256":"%s","published_at":"%s"}\n' \
        "$comp" "$VER" "$ARCH" "$extra" "$file" "$sha" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$tmp"
    repo put --dir "packages/$comp/latest" --file "$tmp" --name "$name" --quiet
    rm -f "$tmp"
}

publish_pkg() {
    local comp="$1" file="$2"
    local src="$PKG_DIR/$file"
    [ -f "$src" ] || { echo "缺少 $src" >&2; return 1; }
    local dir="packages/$comp/$VER/$ARCH"
    repo mkdir --dir "$dir" >/dev/null
    echo "发布 $comp/$VER/$ARCH/$file"
    put_one "$dir" "$src"
    if [ -f "$src.sha256" ]; then
        put_one "$dir" "$src.sha256"
    fi
}

did=0
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = relay ]; then
    f="rtunnel-relay-$VER-$ARCH.tar.gz"
    publish_pkg relay "$f"
    put_latest relay "$ARCH.json" "$f" ""
    did=1
fi
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = client ]; then
    f="rtunnel-client-$VER-$ARCH.tar.gz"
    publish_pkg client "$f"
    put_latest client "$ARCH.json" "$f" ""
    did=1
fi
if [ "$COMPONENT" = all ] || [ "$COMPONENT" = agent ]; then
    for v in lite web; do
        if [ "$VARIANT" != all ] && [ "$VARIANT" != "$v" ]; then continue; fi
        f="rtunnel-agent-$v-$VER-$ARCH.tar.gz"
        if [ ! -f "$PKG_DIR/$f" ]; then
            [ "$VARIANT" = "$v" ] && { echo "缺少 $PKG_DIR/$f" >&2; exit 1; }
            continue
        fi
        publish_pkg agent "$f"
        put_latest agent "$ARCH-$v.json" "$f" ",\"variant\":\"$v\""
    done
    did=1
fi

[ "$did" = 1 ] || { echo "没有可发布的组件" >&2; exit 1; }
echo "完成：$COMPONENT $VER ($ARCH)"
