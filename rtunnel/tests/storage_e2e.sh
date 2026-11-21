#!/bin/bash
# 设备详情页「存储」回归：按路径/层级返回、子项求和、文件节点、小文件聚合、
# 越界校验与 gzip 透传。需要 build/bin/rtunnel-relay 与 build/bin/rtunnel-agent。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${RTUNNEL_BIN:-$ROOT/build/bin}"
RELAY="$BIN/rtunnel-relay"
AGENT="$BIN/rtunnel-agent"
[ -x "$RELAY" ] || { echo "缺少 $RELAY" >&2; exit 1; }
[ -x "$AGENT" ] || { echo "缺少 $AGENT" >&2; exit 1; }

T=$(mktemp -d /tmp/rtunnel-storage-e2e.XXXXXX)
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

# 已知大小的目录树：3M + 2M 大文件、512K 中文件、20 个 1K 小文件、隐藏目录
python3 - "$T/tree" <<'PY'
import os, sys
root = sys.argv[1]
for d in ("a/sub", "b/sub", ".hidden"):
    os.makedirs(os.path.join(root, d), exist_ok=True)
def make(rel, n):
    with open(os.path.join(root, rel), "wb") as f:
        f.truncate(n)
make("a/big1.bin", 3 * 1024 * 1024)
make("a/big2.bin", 2 * 1024 * 1024)
make("a/sub/mid.bin", 512 * 1024)
make("b/sub/deep.bin", 256 * 1024)
for i in range(20):
    make("b/small%02d.dat" % i, 1000)
make(".hidden/h.dat", 500)
PY

"$RELAY" --host 127.0.0.1 --port "$RP" --admin "127.0.0.1:$AP" --admin-token t0k \
    --admin-db "$T/a.db" --admin-file-dir "$T/files" >"$T/relay.log" 2>&1 &
RELAY_PID=$!
sleep 0.6

"$AGENT" --server 127.0.0.1 --port "$RP" --devid tstore \
    --file-root "$T/f" --file-dir "$T/f" --config-file "$T/agent.json" >"$T/agent.log" 2>&1 &
AGENT_PID=$!
sleep 1.0

A="http://127.0.0.1:$AP"
curl -fsS -X POST -H 'Content-Type: application/json' \
    -d '{"token":"t0k","username":"admin","password":"pass12345"}' "$A/api/v1/setup" >/dev/null
curl -fsS -c "$T/c" -X POST -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":"pass12345"}' "$A/api/v1/login" >/dev/null

M="$T/tree"
enc() { python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"; }

api() { # api <query>
    curl -fsS -b "$T/c" "$A/api/v1/agents/tstore/storage?$1"
}

# 1) 分区根：只回一级子项
api "mount=$(enc "$M")&depth=1" >"$T/root.json"
python3 - "$T/root.json" "$M" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
root = sys.argv[2]
assert d["ok"] is True, d
assert d["path"] == root, d.get("path")
paths = [n["p"] for n in d["nodes"]]
assert sorted(paths) == sorted([root, root + "/a", root + "/b", root + "/.hidden"]), paths
assert all(n.get("t") == "d" for n in d["nodes"]), d["nodes"]
# 隐藏目录也统计在内，只是前端默认可以隐藏
assert all(n.get("c") == 1 for n in d["nodes"]), d["nodes"]
print("PASS 分区根只回一级子项")
PY

# 2) 展开 a：直接子项之和等于 a 本身，大文件单列、小文件聚合
api "mount=$(enc "$M")&path=$(enc "$M/a")&depth=1" >"$T/a.json"
python3 - "$T/a.json" "$M" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
root = sys.argv[2]
node = next(n for n in d["nodes"] if n["p"] == root + "/a")
kids = [n for n in d["nodes"] if n["p"] != root + "/a"]
assert sum(n["b"] for n in kids) == node["b"], (node["b"], sum(n["b"] for n in kids))
files = sorted(n["p"] for n in kids if n.get("t") == "f")
assert files == [root + "/a/big1.bin", root + "/a/big2.bin"], files
sub = next(n for n in kids if n["p"] == root + "/a/sub")
assert sub["b"] == 512 * 1024, sub
print("PASS 目录求和与文件节点")
PY

# 3) b 下的小文件聚合成一条
api "mount=$(enc "$M")&path=$(enc "$M/b")&depth=1" >"$T/b.json"
python3 - "$T/b.json" "$M" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
root = sys.argv[2]
agg = [n for n in d["nodes"] if n.get("t") == "o"]
assert len(agg) == 1, d["nodes"]
# 聚合行的 p 是「所属目录 + 不可见后缀」，前端按父目录挂载
assert agg[0]["p"].startswith(root + "/b/"), agg[0]
assert agg[0]["n"] == 20 and agg[0]["b"] == 20 * 1000, agg[0]
print("PASS 小文件按目录聚合")
PY

# 4) 整棵子树：depth=32 取回所有层级
api "mount=$(enc "$M")&path=$(enc "$M/a")&depth=32" >"$T/tree.json"
python3 - "$T/tree.json" "$M" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
root = sys.argv[2]
paths = {n["p"] for n in d["nodes"]}
assert root + "/a/sub/mid.bin" in paths, paths
assert root + "/a/sub" in paths and root + "/a" in paths
assert d["truncated"] is False
print("PASS 整棵子树按 depth 回传")
PY

# 5) 越界与非法路径
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" \
    --get --data-urlencode "mount=$M" --data-urlencode "path=/etc" --data-urlencode "depth=1" \
    "$A/api/v1/agents/tstore/storage")
[ "$code" = "400" ] || { echo "FAIL 越界 path 应 400，实际 $code" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$T/c" \
    --get --data-urlencode "mount=$M" --data-urlencode "path=$M/../etc" --data-urlencode "depth=1" \
    "$A/api/v1/agents/tstore/storage")
[ "$code" = "400" ] || { echo "FAIL .. 路径应 400，实际 $code" >&2; exit 1; }
echo "PASS 越界与非法路径被拒"

# 6) gzip 透传：压缩体解压后与明文一致
curl -fsS -b "$T/c" -H 'Accept-Encoding: gzip' -D "$T/h.txt" -o "$T/plain.gz" \
    "$A/api/v1/agents/tstore/storage?mount=$(enc "$M")&path=$(enc "$M/a")&depth=32"
grep -qi '^content-encoding: gzip' "$T/h.txt" || { echo "FAIL 缺少 Content-Encoding: gzip" >&2; cat "$T/h.txt" >&2; exit 1; }
gzip -dc "$T/plain.gz" >"$T/plain.json"
python3 - "$T/plain.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["ok"] is True and d["nodes"]
print("PASS gzip 透传可解压")
PY

# 7) 不支持 gzip 的客户端拿到明文
curl -fsS -b "$T/c" -o "$T/raw.json" \
    "$A/api/v1/agents/tstore/storage?mount=$(enc "$M")&depth=1"
python3 - "$T/raw.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["ok"] is True
print("PASS 无 gzip 客户端回明文")
PY

echo "storage e2e ok"
