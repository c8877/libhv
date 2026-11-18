#!/bin/bash
# 把 package_agent.sh 生成的部署包上传到 relay 文件仓库的 agent/ 目录（OTA 用）。
#
# 用法：
#   RT=https://192.168.1.133:9443 ATK=atk-xxxx ./deploy/upload_agent.sh [版本]
#   LEGACY=1 ...   # 额外上传旧的裸二进制 .gz，供尚未升级到新 agent 的设备使用
#   INSECURE=1 ... # relay 使用自签证书时跳过校验
#
# 环境变量：
#   RT                  管理台地址，必填
#   ATK                 管理台账号 API Token（atk- 开头），必填
#   RTUNNEL_DEPLOY_DIR  部署目录，默认 build/deploy
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
RT="${RT:?请设置 RT，例如 https://192.168.1.133:9443}"
ATK="${ATK:?请设置 ATK（管理台账号 API Token，atk- 开头）}"
LITE="$DEPLOY/agent/bin/rtunnel-agent"
WEB="$DEPLOY/agent-web/bin/rtunnel-agent"
VER="${1:-}"

if [ -z "$VER" ] && [ -x "$LITE" ]; then
    VER="$("$LITE" --version 2>/dev/null | tr -d '\r\n')"
fi
[ -n "$VER" ] || { echo "无法获取版本，请显式传入：$0 1.4.9" >&2; exit 1; }

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
    [ -f "$src" ] || { echo "缺少 $src" >&2; exit 1; }
    printf '上传 %s (%s) ... ' "$name" "$(du -h "$src" | cut -f1)"
    "${CURL[@]}" -X POST "${AUTH[@]}" --data-binary "@$src" "$BASE/$name?dir=agent" >/dev/null
    echo ok
}

# 新格式：package_agent.sh 已经压好的 tar.gz，原样上传
for v in lite web; do
    pkg="$DEPLOY/packages/rtunnel-agent-$v-$VER.tar.gz"
    if [ -f "$pkg" ]; then
        upload "$pkg" "rtunnel-agent-$v-$VER.tar.gz"
    elif [ "$v" = lite ]; then
        echo "缺少 $pkg，先执行：./deploy/package_agent.sh $VER" >&2
        exit 1
    else
        echo "跳过 web 包（未构建）" >&2
    fi
done

# 旧格式：裸二进制 gzip，仅用于过渡期老 agent
WITH_LEGACY=0
if [ "${LEGACY:-0}" = "1" ]; then
    WITH_LEGACY=1
    upload_legacy() {
        local src="$1" name="$2"
        [ -x "$src" ] || return 0
        local gz="$TMPDIR_UP/$name.gz"
        gzip -9 -n -c "$src" > "$gz"
        upload "$gz" "$name.gz"
    }
    upload_legacy "$LITE" "rtunnel-agent-lite-$VER"
    upload_legacy "$WEB" "rtunnel-agent-web-$VER"
fi

if [ "$WITH_LEGACY" = 1 ]; then
    echo "完成：版本 $VER（含 legacy .gz）"
else
    echo "完成：版本 $VER"
fi
