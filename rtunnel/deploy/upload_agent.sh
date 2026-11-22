#!/bin/bash
# 把 package_agent.sh 生成的部署包上传到 relay 文件仓库的 agent/ 目录（OTA 用）。
#
# 用法：
#   RT=https://192.168.1.133:9443 ATK=atk-xxxx ./deploy/upload_agent.sh [版本]
#   RTUNNEL_ARCH=armv7l ...   # 交叉打包时指定包的目标架构，默认 uname -m
#   INSECURE=1 ...            # relay 使用自签证书时跳过校验
#
# 环境变量：
#   RT                  管理台地址，必填
#   ATK                 管理台账号 API Token（atk- 开头），必填
#   RTUNNEL_DEPLOY_DIR  部署目录，默认 build/deploy
#   RTUNNEL_ARCH        包架构，默认 uname -m
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
RT="${RT:?请设置 RT，例如 https://192.168.1.133:9443}"
# 测试环境默认令牌；轮换后改这一行即可（不想内置就用 ATK=... 覆盖）
ATK="${ATK:-atk-9b56973835029c3e59b4e4be52b4a762a441092655cb67d013f988134b229b29}"
LITE="$DEPLOY/agent/bin/rtunnel-agent"
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"
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

upload() {
    local src="$1" name="$2"
    [ -f "$src" ] || { echo "缺少 $src" >&2; exit 1; }
    printf '上传 %s (%s) ... ' "$name" "$(du -h "$src" | cut -f1)"
    "${CURL[@]}" -X POST "${AUTH[@]}" --data-binary "@$src" "$BASE/$name?dir=agent" >/dev/null
    echo ok
}

for v in lite web; do
    name="rtunnel-agent-$v-$VER-$ARCH.tar.gz"
    pkg="$DEPLOY/packages/$name"
    if [ -f "$pkg" ]; then
        upload "$pkg" "$name"
    elif [ "$v" = lite ]; then
        echo "缺少 $pkg，先执行：RTUNNEL_ARCH=$ARCH ./deploy/package_agent.sh $VER" >&2
        exit 1
    else
        echo "跳过 web 包（未构建）" >&2
    fi
done

echo "完成：版本 $VER ($ARCH)"
