#!/bin/bash
# 设备详情页「网络」回归：TCP/UDP/UNIX/IPC 采集、进程归属、缓存与离线快照。
# 需要 build/bin/rtunnel-relay 与 build/bin/rtunnel-agent。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
RELAY="$BIN/rtunnel-relay"
AGENT="$BIN/rtunnel-agent"
[ -x "$RELAY" ] || { echo "缺少 $RELAY" >&2; exit 1; }
[ -x "$AGENT" ] || { echo "缺少 $AGENT" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-net-e2e.XXXXXX)
RELAY_PID=
AGENT_PID=
IPC_IDS=()
cleanup() {
    [ -n "$AGENT_PID" ] && kill "$AGENT_PID" 2>/dev/null || true
    [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    for id in ${IPC_IDS[@]+"${IPC_IDS[@]}"}; do ipcrm -m "$id" 2>/dev/null || true; done
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

"$AGENT" --server 127.0.0.1 --port "$RP" --devid tnet \
    --file-root "$T/f" --file-dir "$T/f" --config-file "$T/agent.json" >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 1.0

A="http://127.0.0.1:$AP"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"t0k","username":"admin","password":"pass12345"}' "$A/api/v1/setup" >/dev/null
curl -fsS -c "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' "$A/api/v1/login" >/dev/null

# 列表要带上 caps_net，前端据此判断是否需要升级 agent
curl -fsS -b "$T/c" "$A/api/v1/agents" >"$T/agents.json"
python3 - "$T/agents.json" <<'PY'
import json, sys
a = [x for x in json.load(open(sys.argv[1])) if x["devid"] == "tnet"]
assert a, "agent not listed"
assert a[0].get("caps_net") == 1, a[0]
print("caps_net ok")
PY

# TCP
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=tcp&proc=1&refresh=1&page_size=50" >"$T/tcp.json"
SIZE=$(wc -c <"$T/tcp.json")
[ "$SIZE" -lt 32768 ] || { echo "TCP payload $SIZE >= 32768" >&2; exit 1; }
python3 - "$T/tcp.json" "$AP" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
ap = int(sys.argv[2])
assert d["scope"] == "tcp", d["scope"]
assert d.get("cached") is False, d
assert set(d["summary"]) >= {"tcp", "udp", "unix", "ipc"}, d["summary"]
assert d["summary"]["ipc"] == -1, d["summary"]
sec = d["tcp"]
assert sec["entries"], "no tcp entries"
assert sec["total"] >= len(sec["entries"]), sec
for e in sec["entries"]:
    for k in ("local_addr", "local_port", "remote_addr", "remote_port", "state", "inode", "pid", "comm"):
        assert k in e, (k, e)
# 本进程监听的 admin 端口应能反查到进程
assert sec.get("page") == 1 and sec.get("page_size") == 50, sec
assert any(e["pid"] for e in sec["entries"]), "no pid mapped"
print("tcp ok:", len(sec["entries"]), "entries,", sec["matched"], "matched,", sec["listening"], "listening")
PY

# UDP
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=udp&proc=0&refresh=1&page_size=50" >"$T/udp.json"
python3 - "$T/udp.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["scope"] == "udp", d["scope"]
sec = d["udp"]
assert "entries" in sec and "listening" in sec, sec
assert d["proc"] is False, d
for e in sec["entries"][:5]:
    assert e["state"] in ("UNCONN", "ESTAB", "UNKNOWN"), e
print("udp ok:", len(sec["entries"]), "entries")
PY

# UNIX：STREAM/DGRAM 计数与路径
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=unix&proc=1&refresh=1&page_size=50" >"$T/unix.json"
python3 - "$T/unix.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
sec = d["unix"]
assert sec["entries"], "no unix entries"
assert sec["stream"] + sec["dgram"] <= sec["total"], sec
assert sec["listening"] <= sec["total"], sec
named = [e for e in sec["entries"] if e["path"]]
assert named, "no named unix sockets"
for e in sec["entries"][:5]:
    assert e["type"] in ("STREAM", "DGRAM", "SEQPACKET", "OTHER"), e
print("unix ok:", sec["total"], "stream", sec["stream"], "dgram", sec["dgram"])
PY

# IPC
MID=$(ipcmk -M 4096 2>/dev/null | awk '{print $NF}' || true)
[ -n "$MID" ] && IPC_IDS+=("$MID")
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=ipc&proc=1&refresh=1&page_size=50" >"$T/ipc.json"
python3 - "$T/ipc.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
ipc = d["ipc"]
assert ipc["source"] in ("lsipc", "proc"), ipc["source"]
for k in ("shm", "msg", "sem", "limits"):
    assert isinstance(ipc[k], list), (k, type(ipc[k]))
assert d["summary"]["ipc"] == ipc["total"], (d["summary"], ipc["total"])
assert ipc["total"] >= len(ipc["shm"]), ipc["total"]
assert ipc.get("counts") and ipc["counts"]["shm"] >= len(ipc["shm"]), ipc.get("counts")
print("ipc ok:", ipc["source"], "total", ipc["total"], "limits", len(ipc["limits"]))
PY

# 3s 缓存；refresh=1 绕过
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=tcp&proc=0" >"$T/c1.json"
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=tcp&proc=0" >"$T/c2.json"
python3 - "$T/c1.json" "$T/c2.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
assert a.get("cached") is False, a.get("cached")
assert b.get("cached") is True, b.get("cached")
print("cache ok:", b.get("cache_age_ms"), "ms")
PY
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=tcp&proc=0&refresh=1" >"$T/c3.json"
python3 - "$T/c3.json" <<'PY'
import json, sys
assert json.load(open(sys.argv[1])).get("cached") is False
print("refresh bypass ok")
PY

# scope=all 也要在单帧预算内，且四类都带回
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=all&proc=1&refresh=1" >"$T/all.json"
SIZE=$(wc -c <"$T/all.json")
[ "$SIZE" -lt 32768 ] || { echo "all payload $SIZE >= 32768" >&2; exit 1; }
python3 - "$T/all.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for k in ("tcp", "udp", "unix", "ipc"):
    assert k in d, k
assert d["ipc"]["total"] >= 0
print("all ok")
PY

# 非法 scope
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" "$A/api/v1/agents/tnet/net?scope=bogus")
[ "$code" = "400" ] || { echo "bad scope -> $code, want 400" >&2; exit 1; }

# 离线：返回缓存并标记 offline
kill "$AGENT_PID" 2>/dev/null || true
wait "$AGENT_PID" 2>/dev/null || true
AGENT_PID=
# 等 3s 的内存缓存过期，走「离线 + 回退旧快照」分支
sleep 3.5
curl -fsS -b "$T/c" "$A/api/v1/agents/tnet/net?scope=tcp&proc=1&page_size=50" >"$T/off.json"
python3 - "$T/off.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("cached") is True, d
assert d.get("offline") is True, d
assert d["tcp"]["entries"], "cached entries empty"
assert d["ts"] > 0, d["ts"]
print("offline cache ok")
PY

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" "$A/api/v1/agents/ghost/net?scope=tcp")
[ "$code" = "404" ] || { echo "unknown device -> $code, want 404" >&2; exit 1; }

echo "NET E2E PASS"
