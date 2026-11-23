#!/bin/bash
# 设备视图 UI 测试（管理台 + 设备本地页）
#
# 用法:
#   tests/ui/run-ui.sh                 # 跑全部
#   tests/ui/run-ui.sh admin/smoke     # 只跑一个
#   tests/ui/run-ui.sh local/page local/metrics
#   tests/ui/run-ui.sh list            # 列出可用用例
#
# 依赖: 已构建的 build/bin/rtunnel-{relay,agent}、管理台 dist、node + playwright。
# 通过直连本机进程跑，不需要 relay 部署，也不需要令牌。
set -uo pipefail

UI_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$UI_DIR/../../.." && pwd)
BIN=${RTUNNEL_BIN:-$ROOT/build/bin}
DIST=${RTUNNEL_ADMIN_DIST:-$ROOT/rtunnel/relay/admin/web/dist}

SUITES=(
  admin/smoke
  admin/tables
  admin/proc-net
  admin/files
  admin/services
  admin/forms
  admin/metrics
  local/page
  local/metrics
)

if [ "${1:-}" = "list" ]; then
  printf '%s\n' "${SUITES[@]}"
  exit 0
fi

# ---- 依赖检查 -------------------------------------------------------------
missing=0
for f in "$BIN/rtunnel-relay" "$BIN/rtunnel-agent"; do
  if [ ! -x "$f" ]; then
    echo "缺少可执行文件: $f" >&2
    missing=1
  fi
done
if [ ! -f "$DIST/index.html" ]; then
  echo "缺少管理台构建产物: $DIST/index.html" >&2
  echo "先执行: cd rtunnel/relay/admin/web && npm run build" >&2
  missing=1
fi
command -v node >/dev/null || { echo "缺少 node" >&2; missing=1; }
command -v python3 >/dev/null || { echo "缺少 python3（用于挑端口/建库）" >&2; missing=1; }
[ "$missing" = 0 ] || exit 2

if node -e "require('playwright')" >/dev/null 2>&1; then
  :
else
  GLOBAL_MODULES=$(npm root -g 2>/dev/null || true)
  if [ -n "$GLOBAL_MODULES" ] && NODE_PATH="$GLOBAL_MODULES" node -e "require('playwright')" >/dev/null 2>&1; then
    export NODE_PATH="$GLOBAL_MODULES"
  else
    echo "找不到 playwright。安装：npm i -g playwright && npx playwright install chromium" >&2
    exit 2
  fi
fi

# ---- 参数 ----------------------------------------------------------------
RUN=()
if [ $# -eq 0 ]; then
  RUN=("${SUITES[@]}")
else
  for want in "$@"; do
    want=${want%.js}
    hit=""
    for s in "${SUITES[@]}"; do
      if [ "$s" = "$want" ] || [ "$(basename "$s")" = "$want" ]; then
        hit=$s
        break
      fi
    done
    if [ -z "$hit" ]; then
      echo "未知用例: $want（可用: ${SUITES[*]}）" >&2
      exit 2
    fi
    RUN+=("$hit")
  done
fi

# ---- 临时目录与端口 -------------------------------------------------------
free_port() {
  python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

T=$(mktemp -d "${TMPDIR:-/tmp}/rtunnel-ui.XXXXXX")
RP=$(free_port)
AP=$(free_port)
LP=$(free_port)
ART="$T/artifacts"
mkdir -p "$T/files/dir1" "$T/files2" "$ART"

printf 'aaa' > "$T/files/a.txt"
printf 'bbbbbb' > "$T/files/b.txt"
printf 'hello' > "$T/files2/other.txt"

AGENT_PID=""
AGENT2_PID=""
RELAY_PID=""

cleanup() {
  local rc=$?
  [ -n "$AGENT2_PID" ] && kill "$AGENT2_PID" 2>/dev/null
  [ -n "$AGENT_PID" ] && kill "$AGENT_PID" 2>/dev/null
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
  wait 2>/dev/null
  if [ $rc -ne 0 ] || [ "${UI_KEEP:-0}" = "1" ]; then
    echo "日志与截图保留在: $T"
    echo "--- relay.log ---"; tail -15 "$T/relay.log" 2>/dev/null
    echo "--- agent.log ---"; tail -15 "$T/agent.log" 2>/dev/null
  else
    rm -rf "$T"
  fi
}
trap cleanup EXIT

# ---- 启动 relay + 两台 agent ---------------------------------------------
echo "临时目录 $T  relay=$AP admin=$AP local=$LP"

cat > "$T/agent.json" <<JSON
{
  "allow": [22],
  "shell": true,
  "ota": false,
  "reboot": true,
  "poweroff": true,
  "file_dir": "$T/files",
  "fs_enabled": true,
  "fs_root": "$T/files",
  "fs_rw": true,
  "svc_enabled": true,
  "svc_rw": true,
  "local_web": "127.0.0.1:$LP",
  "metrics": {
    "series": [
      { "series": "cpu", "enabled": true, "interval_sec": 1, "points": 120 },
      { "series": "mem", "enabled": true, "interval_sec": 1, "points": 120 },
      { "series": "load", "enabled": true, "interval_sec": 1, "points": 120 },
      { "series": "proc:1:cpu", "enabled": true, "interval_sec": 1, "points": 120 },
      { "series": "proc:1:mem", "enabled": true, "interval_sec": 1, "points": 120 }
    ]
  }
}
JSON

cat > "$T/agent2.json" <<JSON
{
  "allow": [22],
  "shell": true,
  "ota": false,
  "file_dir": "$T/files2",
  "fs_enabled": true,
  "fs_root": "$T/files2",
  "fs_rw": true,
  "svc_enabled": false,
  "svc_rw": false,
  "local_web": false,
  "metrics": { "series": [ { "series": "cpu", "enabled": true, "interval_sec": 5, "points": 60 } ] }
}
JSON

"$BIN/rtunnel-relay" --host 127.0.0.1 --port "$RP" \
  --acl devuser=* --acl dev=* \
  --admin "127.0.0.1:$AP" --admin-token t0k \
  --admin-db "$T/a.db" --admin-file-dir "$T/files" \
  --admin-docroot "$DIST" >"$T/relay.log" 2>&1 &
RELAY_PID=$!

sleep 0.8

"$BIN/rtunnel-agent" --server 127.0.0.1 --port "$RP" --devid uitest \
  --config-file "$T/agent.json" >"$T/agent.log" 2>&1 &
AGENT_PID=$!

"$BIN/rtunnel-agent" --server 127.0.0.1 --port "$RP" --devid uitest2 \
  --config-file "$T/agent2.json" >"$T/agent2.log" 2>&1 &
AGENT2_PID=$!

# 管理面就绪 + 首次 setup
ready=0
for _ in $(seq 1 50); do
  if curl -fsS -o /dev/null "http://127.0.0.1:$AP/" 2>/dev/null; then ready=1; break; fi
  sleep 0.2
done
if [ "$ready" != 1 ]; then
  echo "relay 管理面未启动" >&2
  exit 3
fi
curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"token":"t0k","username":"admin","password":"pass12345"}' \
  "http://127.0.0.1:$AP/api/v1/setup" >/dev/null

# 等两台设备上线（用登录会话，setup 令牌只在初始化前有效）
curl -fsS -c "$T/cookie" -X POST -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"pass12345"}' \
  "http://127.0.0.1:$AP/api/v1/login" >/dev/null
online=0
for _ in $(seq 1 50); do
  n=$(curl -fsS -b "$T/cookie" "http://127.0.0.1:$AP/api/v1/agents" 2>/dev/null \
      | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin)
except Exception:
    print(0); raise SystemExit
print(len([a for a in d if a.get("online")]))' 2>/dev/null)
  if [ "${n:-0}" -ge 2 ]; then online=1; break; fi
  sleep 0.3
done
if [ "$online" != 1 ]; then
  echo "设备未全部上线" >&2
  exit 3
fi

# 采样配置以 relay 的库为准（agent 配置文件只是首次/离线默认值），
# 这里显式下发一套，保证本地页有系统指标 + 一个进程指标。
curl -fsS -b "$T/cookie" -X PUT -H 'Content-Type: application/json' \
  -d '[{"series":"cpu","enabled":true,"interval_sec":1,"points":120},
       {"series":"mem","enabled":true,"interval_sec":1,"points":120},
       {"series":"load","enabled":true,"interval_sec":1,"points":120},
       {"series":"proc:1:cpu","enabled":true,"interval_sec":1,"points":120},
       {"series":"proc:1:mem","enabled":true,"interval_sec":1,"points":120}]' \
  "http://127.0.0.1:$AP/api/v1/agents/uitest/metrics/series/config" >/dev/null
curl -fsS -b "$T/cookie" -X PUT -H 'Content-Type: application/json' \
  -d '[{"series":"cpu","enabled":true,"interval_sec":5,"points":60}]' \
  "http://127.0.0.1:$AP/api/v1/agents/uitest2/metrics/series/config" >/dev/null
sleep 0.5

# 分发任务样本：任务列表和 OTA 页需要多行才能验证排序
python3 - "$T/a.db" <<'PY'
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1])
now = int(time.time() * 1000)
rows = [
    ('ui-1', 'uitest', 'file', '', 'charlie.bin', 3000, '/tmp/charlie.bin', 'success', 30, now - 30000, now - 20000),
    ('ui-2', 'uitest', 'file', '', 'alpha.bin', 1000, '/tmp/alpha.bin', 'failed', 10, now - 20000, now - 10000),
    ('ui-3', 'uitest2', 'file', '', 'bravo.bin', 2000, '/tmp/bravo.bin', 'running', 20, now - 10000, 0),
    ('ui-4', 'uitest', 'ota', '1.0.0', 'fw-a.tar.gz', 400, '/tmp/fw-a.tar.gz', 'success', 0, now - 9000, now - 8000),
    ('ui-5', 'uitest2', 'ota', '1.1.0', 'fw-b.tar.gz', 500, '/tmp/fw-b.tar.gz', 'failed', 0, now - 8000, now - 7000),
]
for batch, devid, kind, ver, name, size, path, status, retry, created, finished in rows:
    db.execute(
        'INSERT INTO distribution_tasks(batch_id,devid,kind,target_version,file_name,file_size,'
        'target_path,status,retry_count,sent,created_ms,finished_ms) '
        'VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
        (batch, devid, kind, ver, name, size, path, status, retry, size, created, finished))
db.commit()
db.close()
PY

# ---- 跑用例 --------------------------------------------------------------
export A="http://127.0.0.1:$AP"
export L="http://127.0.0.1:$LP"
export T ART DEV=uitest DEV2=uitest2
export UI_ARTIFACTS="$ART"

rc=0
for s in "${RUN[@]}"; do
  echo
  echo "=== $s ==="
  if ! node "$UI_DIR/$s.js"; then
    rc=1
    echo "--- $s 失败 ---"
  fi
done

echo
if [ $rc -eq 0 ]; then
  echo "全部用例通过（${RUN[*]}）"
else
  echo "有失败用例"
fi
exit $rc
