#!/bin/sh
# 无 systemd 环境下的 relay 重启脚本（容器、/tmp 手工部署、精简系统）。
# 有 systemd 时不要用它，直接 systemctl restart rtunnel-relay。
#
# 用法：
#   ./restart.sh [--prefix P] [--args-file F] [--log F]
#   ./restart.sh --stop
#
# 参数文件每行一个参数（默认 $PREFIX/etc/relay.args），日志追加到 $PREFIX/relay.log。
# 退出码：0 成功；3 启动超时；4 端口仍被占用；5 参数文件不存在
# 参数文件支持两种写法，默认优先 systemd 用的 relay.env：
#   RELAY_ARGS="--port 18443 --admin 0.0.0.0:9443"
# 或纯参数文件（每行一个参数），见 etc/relay.args.example。
set -eu

PREFIX=${RTUNNEL_RELAY_PREFIX:-/opt/rtunnel/relay}
ARGS_FILE=${RTUNNEL_RELAY_ARGS:-}
LOG=${RTUNNEL_RELAY_LOG:-}
STOP=0
WAIT=${RTUNNEL_RELAY_STOP_WAIT:-15}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --args-file) ARGS_FILE=$2; shift 2 ;;
        --log) LOG=$2; shift 2 ;;
        --stop) STOP=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$ARGS_FILE" ]; then
    if [ -f "$PREFIX/etc/relay.env" ]; then
        ARGS_FILE="$PREFIX/etc/relay.env"
    else
        ARGS_FILE="$PREFIX/etc/relay.args"
    fi
fi
[ -n "$LOG" ] || LOG="$PREFIX/relay.log"
BIN="$PREFIX/bin/rtunnel-relay"

# 按进程名匹配：命令行里可能是 ./relay/bin/rtunnel-relay，也可能是绝对路径，
# 用 -f 匹配路径会漏掉相对路径启动的实例（B 机器上踩过）。
pids() {
    if command -v pidof >/dev/null 2>&1; then
        pidof rtunnel-relay 2>/dev/null || true
    else
        pgrep -x rtunnel-relay 2>/dev/null || true
    fi
}

# 只发 SIGTERM：relay 会 drain 现有流再退出；端口占着就先等它让位
stop_all() {
    p=$(pids)
    [ -n "$p" ] || return 0
    kill $p 2>/dev/null || true
    i=0
    while [ "$i" -lt "$WAIT" ]; do
        [ -z "$(pids)" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    # 超时仍未退出：再等不到就只能强杀，避免端口永久占用
    p=$(pids)
    [ -n "$p" ] && kill -9 $p 2>/dev/null || true
    sleep 1
}

stop_all
[ "$STOP" = 1 ] && { echo "stopped"; exit 0; }

[ -x "$BIN" ] || { echo "缺少 $BIN" >&2; exit 5; }
[ -f "$ARGS_FILE" ] || { echo "缺少参数文件 $ARGS_FILE" >&2; exit 5; }

# 参数文件里不要写 shell 元字符；这里按空白拆分是刻意的。
# RELAY_ARGS="..." 形式（systemd EnvironmentFile）去掉前缀和引号后同样处理。
set --
while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    case "$line" in
        RELAY_ARGS=*)
            line=${line#RELAY_ARGS=}
            line=${line#\"}
            line=${line%\"}
            ;;
    esac
    for a in $line; do set -- "$@" "$a"; done
done < "$ARGS_FILE"

mkdir -p "$(dirname "$LOG")"
nohup "$BIN" "$@" >> "$LOG" 2>&1 &
sleep 2

p=$(pids)
if [ -z "$p" ]; then
    echo "启动失败，检查 $LOG" >&2
    exit 3
fi

# 端口从参数里取，没有就跳过检查
PORT=
prev=
for a in "$@"; do
    if [ "$prev" = "--port" ]; then PORT=$a; break; fi
    prev=$a
done
if [ -n "$PORT" ] && command -v ss >/dev/null 2>&1; then
    ss -ltn 2>/dev/null | grep -q ":$PORT " || { echo "端口 $PORT 未监听，检查 $LOG" >&2; exit 4; }
fi

echo "started pid=$p port=${PORT:-?}"
