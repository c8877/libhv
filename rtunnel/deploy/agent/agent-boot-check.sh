#!/bin/sh
# OTA 启动看门狗（systemd ExecStartPre）。
# pending 由 install.sh 在切软链前写入：prev <TAB> new <TAB> attempts <TAB> ts
# 连续 OTA_BOOT_TRIES 次都没等到 agent 自己清掉 pending，就把软链切回 prev。
set -u

PREFIX=${RTUNNEL_AGENT_PREFIX:-/opt/rtunnel/agent}
BIN=$PREFIX/bin
STATE=$PREFIX/state
PENDING=$STATE/pending
LINK=$BIN/rtunnel-agent
MAX=${OTA_BOOT_TRIES:-3}
LOG=$STATE/ota.log

[ -f "$PENDING" ] || exit 0

line=$(head -n 1 "$PENDING" 2>/dev/null || true)
prev=$(printf '%s' "$line" | cut -f1)
new=$(printf '%s' "$line" | cut -f2)
tries=$(printf '%s' "$line" | cut -f3)

case "$tries" in
    ''|*[!0-9]*) tries=0 ;;
esac
tries=$((tries + 1))

cur=$(readlink "$LINK" 2>/dev/null || true)
cur=${cur##*/}

log() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || true)" "$*" >> "$LOG" 2>/dev/null || true
}

# 软链已经不在新版本上（人工回滚或 install.sh 中途失败），清掉标记即可
if [ "$cur" != "$new" ]; then
    rm -f "$PENDING"
    exit 0
fi

if [ "$tries" -gt "$MAX" ]; then
    if [ -n "$prev" ] && [ -f "$BIN/$prev" ]; then
        tmp=$BIN/.rtunnel-agent.$$
        rm -f "$tmp"
        if ln -s "$prev" "$tmp" 2>/dev/null; then
            mv -f "$tmp" "$LINK"
            log "rollback $new -> $prev after $((tries - 1)) starts"
        else
            log "rollback link create failed ($new -> $prev)"
        fi
    else
        log "no rollback target for $new after $tries starts"
    fi
    rm -f "$PENDING"
    exit 0
fi

printf '%s\t%s\t%s\t%s\n' "$prev" "$new" "$tries" "$(date +%s 2>/dev/null || echo 0)" > "$PENDING"
log "start attempt $tries/$MAX for $new"
exit 0
