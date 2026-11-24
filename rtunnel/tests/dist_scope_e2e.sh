#!/bin/bash
# 分发/OTA 设备范围回归：受限账号只能看到并操作自己权限组内的设备。
# 需要 build/bin/rtunnel-relay 和一个部署包。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
PKGDIR="${1:-$ROOT/build/deploy/packages}"
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"
RELAY="$BIN/rtunnel-relay"
[ -x "$RELAY" ] || { echo "缺少 $RELAY" >&2; exit 1; }
VER="${2:-}"
if [ -z "$VER" ]; then
    pkg=$(ls -1t "$PKGDIR"/rtunnel-agent-lite-*-"${ARCH}".tar.gz 2>/dev/null | head -1 || true)
    [ -n "$pkg" ] || { echo "缺少部署包" >&2; exit 1; }
    VER=$(basename "$pkg" | sed -E 's/^rtunnel-agent-lite-(.+)-[^-]+\.tar\.gz$/\1/')
fi
PKG="$PKGDIR/rtunnel-agent-lite-$VER-$ARCH.tar.gz"
[ -f "$PKG" ] || { echo "缺少 $PKG" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-scope-e2e.XXXXXX)
RELAY_PID=
AGENT_PID=
cleanup() {
    [ -n "$AGENT_PID" ] && kill "$AGENT_PID" 2>/dev/null || true
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT

free_port() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}
RP=$(free_port)
AP=$(free_port)
mkdir -p "$T/agent" "$T/etc" "$T/files" "$T/repo"
tar -C "$T" -xzf "$PKG"
PKGROOT="$T/rtunnel-agent-lite-$VER-$ARCH"
RTUNNEL_CONF_DIR="$T/etc" sh "$PKGROOT/install.sh" \
    --pkg-dir "$PKGROOT" --variant lite --version "$VER" --prefix "$T/agent" --no-units >/dev/null

"$RELAY" --host 127.0.0.1 --port "$RP" --admin 127.0.0.1:"$AP" \
    --admin-token admintok --admin-db "$T/admin.db" --admin-file-dir "$T/repo" >"$T/relay.log" 2>&1 &
RELAY_PID=$!
sleep 0.6
RTUNNEL_AGENT_PREFIX="$T/agent" "$T/agent/bin/rtunnel-agent" \
    --server 127.0.0.1 --port "$RP" --devid owndev \
    --file-dir "$T/files" --config-file "$T/etc/rtunnel-agent.json" >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 0.8

# admin 初始化
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"admintok","username":"admin","password":"pass12345"}' \
    "http://127.0.0.1:$AP/api/v1/setup" >/dev/null
curl -fsS -c "$T/admin.cookie" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' \
    "http://127.0.0.1:$AP/api/v1/login" >/dev/null
# 让在线设备进入 res_devices
curl -fsS -b "$T/admin.cookie" "http://127.0.0.1:$AP/api/v1/agents" >/dev/null
curl -fsS -b "$T/admin.cookie" -X POST "http://127.0.0.1:$AP/api/v1/files/agent?action=mkdir" >/dev/null
curl -fsS -b "$T/admin.cookie" -X POST --data-binary @"$PKG" \
    "http://127.0.0.1:$AP/api/v1/files/$(basename "$PKG")?dir=agent" >/dev/null

# 创建受限账号与权限组
curl -fsS -b "$T/admin.cookie" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"limited","password":"pass12345","role":"viewer"}' \
    "http://127.0.0.1:$AP/api/v1/users" >/dev/null
LUID=$(curl -fsS -b "$T/admin.cookie" "http://127.0.0.1:$AP/api/v1/users" |
    python3 -c 'import json,sys;a=json.load(sys.stdin);print(next(u["id"] for u in a if u["username"]=="limited"))')
curl -fsS -b "$T/admin.cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"name\":\"scope\",\"resources\":[\"device.view\",\"dist.read\",\"dist.write\"],\"devices\":[\"owndev\"],\"members\":[{\"type\":\"user\",\"id\":\"$LUID\"}]}" \
    "http://127.0.0.1:$AP/api/v1/permission/groups" >/dev/null

curl -fsS -c "$T/limited.cookie" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"limited","password":"pass12345"}' \
    "http://127.0.0.1:$AP/api/v1/login" >/dev/null

# admin 为第三方设备建一条 OTA 任务（离线入队）
ADMIN_TASK=$(curl -fsS -b "$T/admin.cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"file\":\"agent/$(basename "$PKG")\",\"kind\":\"ota\",\"version\":\"$VER\",\"devids\":[\"thirddev\"],\"max_retries\":0}" \
    "http://127.0.0.1:$AP/api/v1/distributions")
TID=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1])["tasks"][0])' "$ADMIN_TASK")

# 1) 受限账号向非授权设备推任务必须 403
code=$(curl -sS -o /dev/null -w '%{http_code}' -b "$T/limited.cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"file\":\"agent/$(basename "$PKG")\",\"kind\":\"ota\",\"version\":\"$VER\",\"devids\":[\"thirddev\"],\"force\":true}" \
    "http://127.0.0.1:$AP/api/v1/distributions")
[ "$code" = "403" ] || { echo "expected 403 for foreign devid, got $code"; exit 1; }

# 2) 受限账号看不到第三方任务
LIST=$(curl -fsS -b "$T/limited.cookie" "http://127.0.0.1:$AP/api/v1/distributions?kind=ota&per_page=100")
echo "$LIST" | grep -q thirddev && { echo "foreign task leaked in list"; exit 1; }
# 3) 直接按 id 取也必须是 403
code=$(curl -sS -o /dev/null -w '%{http_code}' -b "$T/limited.cookie" \
    "http://127.0.0.1:$AP/api/v1/distributions/$TID")
[ "$code" = "403" ] || { echo "expected 403 for foreign task, got $code"; exit 1; }
# 4) 设备列表只含授权设备
DEVS=$(curl -fsS -b "$T/limited.cookie" "http://127.0.0.1:$AP/api/v1/devices")
echo "$DEVS" | grep -q owndev || { echo "own device missing"; exit 1; }
echo "$DEVS" | grep -q thirddev && { echo "foreign device leaked"; exit 1; }

# 5) 向自己有权限的设备创建任务应成功
code=$(curl -sS -o /dev/null -w '%{http_code}' -b "$T/limited.cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"file\":\"agent/$(basename "$PKG")\",\"kind\":\"ota\",\"version\":\"$VER\",\"devids\":[\"owndev\"],\"force\":true,\"max_retries\":0}" \
    "http://127.0.0.1:$AP/api/v1/distributions")
[ "$code" = "202" ] || { echo "expected 202 for own devid, got $code"; exit 1; }

echo "DIST SCOPE E2E PASS"
