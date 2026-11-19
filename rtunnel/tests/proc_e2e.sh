#!/bin/bash
# 设备详情页「进程」回归：列表/摘要/排序/搜索、详情、信号、越权与参数校验。
# 需要 build/bin/rtunnel-relay 与 build/bin/rtunnel-agent。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
RELAY="$BIN/rtunnel-relay"
AGENT="$BIN/rtunnel-agent"
[ -x "$RELAY" ] || { echo "缺少 $RELAY" >&2; exit 1; }
[ -x "$AGENT" ] || { echo "缺少 $AGENT" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-proc-e2e.XXXXXX)
RELAY_PID=
AGENT_PID=
SLEEP_PID=
cleanup() {
    [ -n "$SLEEP_PID" ] && kill "$SLEEP_PID" 2>/dev/null || true
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

"$AGENT" --server 127.0.0.1 --port "$RP" --devid tproc \
    --file-root "$T/f" --file-dir "$T/f" --config-file "$T/agent.json" >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 1.0

A="http://127.0.0.1:$AP"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"t0k","username":"admin","password":"pass12345"}' "$A/api/v1/setup" >/dev/null
curl -fsS -c "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' "$A/api/v1/login" >/dev/null

# 列表要带 caps_proc，前端据此判断是否需要升级 agent
curl -fsS -b "$T/c" "$A/api/v1/agents" >"$T/agents.json"
python3 - "$T/agents.json" <<'PY'
import json, sys
a = [x for x in json.load(open(sys.argv[1])) if x["devid"] == "tproc"]
assert a, "agent not listed"
assert a[0].get("caps_proc") == 1, a[0]
print("caps_proc ok")
PY

# 造一个可搜索、可结束的目标进程
sleep 777 &
SLEEP_PID=$!
sleep 0.3

curl -fsS -b "$T/c" "$A/api/v1/agents/tproc/procs?sort=cpu&page=1&page_size=50" >"$T/list.json"
python3 - "$T/list.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("op") == "list", d.get("op")
assert d.get("error") is None, d.get("error")
s = d["summary"]
for k in ("total", "running", "sleeping", "threads", "load1", "mem_total_kb", "mem_used_kb"):
    assert k in s, k
assert s["total"] > 0 and s["threads"] > 0, s
assert d["sample_ms"] == 300, d["sample_ms"]
assert d["page"] == 1 and d["page_size"] == 50, (d.get("page"), d.get("page_size"))
assert d["pages"] >= 1 and d["matched"] == s["matched"], (d.get("pages"), d.get("matched"))
e = d["entries"]
assert e, "no entries"
for x in e[:20]:
    for k in ("pid", "user", "comm", "state", "cpu_pct", "rss_kb", "threads", "cmd", "start_ms"):
        assert k in x, (k, x)
    assert x["pid"] > 0 and x["cpu_pct"] >= 0, x
print("list ok:", s["total"], "procs,", len(e), "entries")
PY

# 搜索：q 命中目标进程
curl -fsS -b "$T/c" "$A/api/v1/agents/tproc/procs?q=sleep%20777" >"$T/q.json"
python3 - "$T/q.json" "$SLEEP_PID" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
pid = int(sys.argv[2])
hits = [x for x in d["entries"] if x["pid"] == pid]
assert hits, [x["pid"] for x in d["entries"][:10]]
assert "sleep 777" in hits[0]["cmd"], hits[0]["cmd"]
print("search ok:", hits[0]["comm"], hits[0]["cmd"])
PY

# 排序：按内存降序
curl -fsS -b "$T/c" "$A/api/v1/agents/tproc/procs?sort=mem&page=1&page_size=20" >"$T/mem.json"
python3 - "$T/mem.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
r = [x["rss_kb"] for x in d["entries"]]
assert r == sorted(r, reverse=True), r[:8]
print("sort mem ok")
PY

# 详情
curl -fsS -b "$T/c" "$A/api/v1/agents/tproc/procs/$SLEEP_PID" >"$T/detail.json"
python3 - "$T/detail.json" "$SLEEP_PID" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
pid = int(sys.argv[2])
assert d.get("pid") == pid, d
assert "sleep 777" in d.get("cmd", ""), d.get("cmd")
assert d.get("exe"), d
assert d.get("state"), d
print("detail ok:", d["pid"], d["exe"])
PY

# 信号：SIGTERM 结束目标进程
curl -fsS -b "$T/c" -X POST -H 'Content-Type: application/json' \
    -d "{\"pid\":$SLEEP_PID,\"signal\":15}" "$A/api/v1/agents/tproc/procs/signal" >"$T/sig.json"
python3 - "$T/sig.json" "$SLEEP_PID" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("ok") is True, d
assert d.get("pid") == int(sys.argv[2]), d
assert d.get("signal") == 15, d
print("signal ok")
PY
sleep 0.4
kill -0 "$SLEEP_PID" 2>/dev/null && { echo "target still alive" >&2; exit 1; }
SLEEP_PID=
echo "target terminated"

# 参数校验
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"pid":1,"signal":15}' "$A/api/v1/agents/tproc/procs/signal")
[ "$code" = "400" ] || { echo "pid 1 -> $code, want 400" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" -X POST -H 'Content-Type: application/json' \
    -d "{\"pid\":$$,\"signal\":99}" "$A/api/v1/agents/tproc/procs/signal")
[ "$code" = "400" ] || { echo "signal 99 -> $code, want 400" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" "$A/api/v1/agents/tproc/procs/0")
[ "$code" = "400" ] || { echo "pid 0 detail -> $code, want 400" >&2; exit 1; }
echo "validation ok"

# 权限：只给 device.view 的账号看不到进程、也不能发信号
curl -fsS -b "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"limited","password":"pass12345","role":"viewer"}' \
    "$A/api/v1/users" >/dev/null
LUID=$(curl -fsS -b "$T/c" "$A/api/v1/users" |
    python3 -c 'import json,sys;a=json.load(sys.stdin);print(next(u["id"] for u in a if u["username"]=="limited"))')
curl -fsS -b "$T/c" -X POST -H 'Content-Type: application/json' \
    -d "{\"name\":\"viewonly\",\"resources\":[\"device.view\"],\"devices\":[\"tproc\"],\"members\":[{\"type\":\"user\",\"id\":\"$LUID\"}]}" \
    "$A/api/v1/permission/groups" >/dev/null
curl -fsS -c "$T/limited.cookie" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"limited","password":"pass12345"}' "$A/api/v1/login" >/dev/null

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/limited.cookie" "$A/api/v1/agents/tproc/procs")
[ "$code" = "403" ] || { echo "limited list -> $code, want 403" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/limited.cookie" -X POST \
    -H 'Content-Type: application/json' -d "{\"pid\":$$,\"signal\":15}" \
    "$A/api/v1/agents/tproc/procs/signal")
[ "$code" = "403" ] || { echo "limited signal -> $code, want 403" >&2; exit 1; }
echo "permission ok"

echo "PROC E2E PASS"
