#!/bin/bash
# 把 agent 的 lite / web 二进制 gzip 压缩后上传到 relay 文件仓库的 agent/ 目录（OTA 用）。
# 上传名形如 rtunnel-agent-lite-1.4.3.gz，OTA 页选择后会自动填入目标版本，
# 设备侧收到后解压、校验 --version 再安装。
#
# 用法：
#   RT=https://192.168.1.133:9443 ATK=atk-xxxx ./deploy/upload_agent.sh [版本]
#   INSECURE=1 ...        # relay 使用自签证书时跳过校验
#
# 环境变量：
#   RT                    管理台地址，必填
#   ATK                   管理台账号 API Token（atk- 开头），必填
#   RTUNNEL_DEPLOY_DIR    部署目录，默认 build/deploy
#
# 上传前先构建：cmake --build build --target rtunnel-deploy rtunnel-deploy-web
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
RT="${RT:?请设置 RT，例如 https://192.168.1.133:9443}"
ATK="${ATK:?请设置 ATK（管理台账号 API Token，atk- 开头）}"
LITE="$DEPLOY/agent/bin/rtunnel-agent"
WEB="$DEPLOY/agent-web/bin/rtunnel-agent"
VER="${1:-}"

if [ -z "$VER" ]; then
    VER="$("$LITE" --version 2>/dev/null | tr -d '\r\n')"
fi
if [ -z "$VER" ]; then
    echo "无法获取 agent 版本，请显式传入：$0 1.4.3" >&2
    exit 1
fi

CURL=(curl -fsS)
AUTH=(-H "Authorization: Bearer $ATK")
[ "${INSECURE:-0}" = "1" ] && CURL+=(-k)

BASE="$RT/api/v1/files"
# 建目录允许 409（已存在），这里不用 -f，避免把预期冲突打印成错误
MK=(curl -sS -o /dev/null -w '%{http_code}')
[ "${INSECURE:-0}" = "1" ] && MK+=(-k)
code="$("${MK[@]}" -X POST "${AUTH[@]}" "$BASE/agent?action=mkdir" || true)"
case "$code" in
    200|201|409) ;;
    *) echo "创建 agent/ 失败：HTTP $code" >&2; exit 1 ;;
esac

TMPDIR_UP="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_UP"' EXIT

upload() {
    local src="$1" name="$2"
    if [ ! -f "$src" ]; then
        echo "缺少 $src，请先构建并执行 rtunnel-deploy 与 rtunnel-deploy-web" >&2
        exit 1
    fi
    local gz="$TMPDIR_UP/$name.gz"
    # -n：不写入时间戳，同样的二进制重复上传可得到一致的 gzip
    gzip -9 -n -c "$src" > "$gz"
    printf '上传 %s (%s → %s) ... ' "$name.gz" "$(du -h "$src" | cut -f1)" "$(du -h "$gz" | cut -f1)"
    "${CURL[@]}" -X POST "${AUTH[@]}" --data-binary "@$gz" "$BASE/$name.gz?dir=agent" >/dev/null
    echo ok
}

upload "$LITE" "rtunnel-agent-lite-$VER"
upload "$WEB" "rtunnel-agent-web-$VER"

echo "完成：agent/rtunnel-agent-lite-$VER.gz、agent/rtunnel-agent-web-$VER.gz"
