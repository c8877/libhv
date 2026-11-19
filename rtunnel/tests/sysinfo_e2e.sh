#!/bin/bash
# 设备详情页存储统计回归：全量/增量的挂载字段、离线缓存标记。
# 需要 build/bin/rtunnel-relay 与 build/bin/rtunnel-agent。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
RELAY="$BIN/rtunnel-relay"
AGENT="$BIN/rtunnel-agent"
[ -x "$RELAY" ] || { echo "缺少 $RELAY" >&2; exit 1; }
[ -x "$AGENT" ] || { echo "缺少 $AGENT" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-sysinfo-e2e.XXXXXX)
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
mkdir -p "$T/files"

"$RELAY" --host 127.0.0.1 --port "$RP" --admin "127.0.0.1:$AP" --admin-token t0k \
    --admin-db "$T/a.db" --admin-file-dir "$T/files" >"$T/relay.log" 2>&1 &
RELAY_PID=$!
sleep 0.6

"$AGENT" --server 127.0.0.1 --port "$RP" --devid tdisk \
    --file-root "$T/f" --file-dir "$T/f" >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 0.8

A="http://127.0.0.1:$AP"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"t0k","username":"admin","password":"pass12345"}' "$A/api/v1/setup" >/dev/null
curl -fsS -c "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' "$A/api/v1/login" >/dev/null

curl -fsS -b "$T/c" "$A/api/v1/agents/tdisk/sysinfo?mode=full" >"$T/full.json"
python3 - "$T/full.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
s = d["sysinfo"]
assert d["mode"] == "full", d["mode"]
assert not d.get("cached"), d
mounts = s.get("mounts") or []
assert mounts, "no mounts"
for m in mounts:
    assert "total_kb" in m and "used_kb" in m and "avail_kb" in m, m
    assert "free_kb" not in m, m
    assert 0 <= m["used_kb"] <= m["total_kb"], m
    assert m["used_kb"] + m["avail_kb"] <= m["total_kb"] + 1024, m
print("full mounts ok:", len(mounts))
PY

curl -fsS -b "$T/c" "$A/api/v1/agents/tdisk/sysinfo?mode=inc" >"$T/inc.json"
python3 - "$T/inc.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["mode"] == "inc", d["mode"]
mounts = d["sysinfo"].get("mounts") or []
assert mounts, "no incremental mounts"
for m in mounts:
    assert "used_kb" in m and "avail_kb" in m and "mount" in m, m
print("incremental mounts ok:", len(mounts))
PY

kill "$AGENT_PID" 2>/dev/null || true
wait "$AGENT_PID" 2>/dev/null || true
AGENT_PID=
sleep 0.8
curl -fsS -b "$T/c" "$A/api/v1/agents/tdisk/sysinfo?mode=inc" >"$T/off.json"
python3 - "$T/off.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["mode"] == "full", d["mode"]
assert d.get("cached") is True, d
assert d["ts"] == 0, d["ts"]
assert d["sysinfo"].get("mounts"), "cached sysinfo empty"
print("offline cache ok")
PY

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" "$A/api/v1/agents/ghost/sysinfo?mode=full")
[ "$code" = "404" ] || { echo "unknown device -> $code, want 404" >&2; exit 1; }
echo "SYSINFO E2E PASS"
