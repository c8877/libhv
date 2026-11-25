#!/bin/bash
# OTA 本机回归：relay + 管理 API + 文件上传 + distributions + tar.gz 安装 + 重启确认。
# 需要先构建 relay 和部署包：
#   cmake --build build --target rtunnel-relay
#   ./deploy/package_agent.sh <ver>
#
# 用法：bash tests/ota_e2e.sh [包目录] [版本]
# 环境：RTUNNEL_BIN 默认 build/bin；RTUNNEL_ARCH 默认 uname -m
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
PKGDIR="${1:-$ROOT/build/deploy/packages}"
ARCH="${RTUNNEL_ARCH:-$(uname -m)}"
VER="${2:-}"
RELAY="$BIN/rtunnel-relay"

[ -x "$RELAY" ] || { echo "缺少 $RELAY，先构建 rtunnel-relay" >&2; exit 1; }

if [ -z "$VER" ]; then
    pkg=$(ls -1t "$PKGDIR"/rtunnel-agent-lite-*-"${ARCH}".tar.gz 2>/dev/null | head -1 || true)
    [ -n "$pkg" ] || { echo "缺少 $PKGDIR/rtunnel-agent-lite-*-$ARCH.tar.gz，先打包" >&2; exit 1; }
    VER=$(basename "$pkg" | sed -E 's/^rtunnel-agent-lite-(.+)-[^-]+\.tar\.gz$/\1/')
fi
PKG="$PKGDIR/rtunnel-agent-lite-$VER-$ARCH.tar.gz"
[ -f "$PKG" ] || { echo "缺少 $PKG" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-ota-e2e.XXXXXX)
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

echo "== install $VER ($ARCH) into $T/agent"
tar -C "$T" -xzf "$PKG"
PKGROOT="$T/rtunnel-agent-$VER-$ARCH"
RTUNNEL_CONF_DIR="$T/etc" sh "$PKGROOT/install.sh" \
    --pkg-dir "$PKGROOT" --variant lite --version "$VER" --prefix "$T/agent" --no-units >/dev/null

echo "== relay $RP / admin $AP"
"$RELAY" --host 127.0.0.1 --port "$RP" --admin 127.0.0.1:"$AP" \
    --admin-token admintok --admin-db "$T/admin.db" --admin-file-dir "$T/repo" >"$T/relay.log" 2>&1 &
RELAY_PID=$!
sleep 0.6

echo "== agent"
RTUNNEL_AGENT_PREFIX="$T/agent" "$T/agent/bin/rtunnel-agent" \
    --server 127.0.0.1 --port "$RP" --devid otae2e \
    --file-dir "$T/files" --config-file "$T/etc/rtunnel-agent.json" \
    >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 0.8

curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"admintok","username":"admin","password":"pass12345"}' \
    "http://127.0.0.1:$AP/api/v1/setup" >/dev/null
curl -fsS -c "$T/cookie" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' \
    "http://127.0.0.1:$AP/api/v1/login" >/dev/null
curl -fsS -b "$T/cookie" -X POST "http://127.0.0.1:$AP/api/v1/files/agent?action=mkdir" >/dev/null
curl -fsS -b "$T/cookie" -X POST --data-binary @"$PKG" \
    "http://127.0.0.1:$AP/api/v1/files/$(basename "$PKG")?dir=agent" >/dev/null

echo "== create ota task (force reinstall $VER)"
RESP=$(curl -fsS -b "$T/cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"file\":\"agent/$(basename "$PKG")\",\"kind\":\"ota\",\"version\":\"$VER\",\"devids\":[\"otae2e\"],\"force\":true,\"max_retries\":0}" \
    "http://127.0.0.1:$AP/api/v1/distributions")
BATCH=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]).get("batch_id",""))' "$RESP")

ST=?
for _ in $(seq 1 60); do
    S=$(curl -fsS -b "$T/cookie" \
        "http://127.0.0.1:$AP/api/v1/distributions?kind=ota&batch=$BATCH")
    ST=$(python3 -c 'import json,sys;d=json.loads(sys.argv[1]);a=d.get("items") or [];print(a[0]["status"] if a else "?")' "$S" 2>/dev/null || echo '?')
    case "$ST" in success|failed|timeout|conflict|unsupported|canceled) break ;; esac
    sleep 1
done

echo "status: $ST"
[ "$ST" = "success" ] || { echo "OTA failed"; tail -30 "$T/agent.log"; exit 1; }
[ "$(readlink "$T/agent/bin/rtunnel-agent")" = "rtunnel-agent-lite-$VER" ] || { echo "symlink mismatch"; exit 1; }
[ "$("$T/agent/bin/rtunnel-agent" --version)" = "$VER" ] || { echo "version mismatch"; exit 1; }
[ ! -f "$T/agent/state/pending" ] || { echo "pending not cleared"; exit 1; }
curl -fsS -b "$T/cookie" "http://127.0.0.1:$AP/api/v1/agents" | grep -q "\"$VER\"" || {
    echo "relay did not report $VER"; exit 1
}

echo "OTA E2E PASS ($VER, $ARCH)"

echo "== negative: wrong arch package must fail without touching symlink"
WRONG="$T/rtunnel-agent-lite-$VER-mips64"
cp -a "$PKGROOT" "$WRONG"
sed -i "s/\"arch\":\"$ARCH\"/\"arch\":\"mips64\"/" "$WRONG/manifest.json"
tar --format=ustar -C "$T" -cf - "$(basename "$WRONG")" | gzip -9 -n >"$T/wrong.tar.gz"
curl -fsS -b "$T/cookie" -X POST --data-binary @"$T/wrong.tar.gz" \
    "http://127.0.0.1:$AP/api/v1/files/rtunnel-agent-lite-$VER-mips64.tar.gz?dir=agent" >/dev/null
RESP=$(curl -fsS -b "$T/cookie" -X POST -H 'Content-Type: application/json' \
    -d "{\"file\":\"agent/rtunnel-agent-lite-$VER-mips64.tar.gz\",\"kind\":\"ota\",\"version\":\"$VER\",\"devids\":[\"otae2e\"],\"force\":true,\"max_retries\":0}" \
    "http://127.0.0.1:$AP/api/v1/distributions")
BATCH=$(python3 -c 'import json,sys;print(json.loads(sys.argv[1]).get("batch_id",""))' "$RESP")
ST=?
for _ in $(seq 1 30); do
    S=$(curl -fsS -b "$T/cookie" \
        "http://127.0.0.1:$AP/api/v1/distributions?kind=ota&batch=$BATCH")
    ST=$(python3 -c 'import json,sys;d=json.loads(sys.argv[1]);a=d.get("items") or [];print(a[0]["status"] if a else "?")' "$S" 2>/dev/null || echo '?')
    case "$ST" in success|failed|timeout|conflict|unsupported|canceled) break ;; esac
    sleep 1
done
echo "wrong-arch status: $ST"
[ "$ST" = "failed" ] || { echo "wrong arch was not rejected"; exit 1; }
[ "$(readlink "$T/agent/bin/rtunnel-agent")" = "rtunnel-agent-lite-$VER" ] || { echo "symlink changed"; exit 1; }
echo "WRONG ARCH REJECT PASS"
