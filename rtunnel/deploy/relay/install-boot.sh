#!/bin/sh
# 无 systemd 环境下的开机自启与存活看护：给当前用户装 crontab 条目。
# 有 systemd 时不要用（systemctl enable 才是正路）。
#
# 用法：
#   ./install-boot.sh [--prefix P] [--log F] [--every N] [--print] [--remove]
#
# 安装两条：
#   @reboot      启动 relay
#   */N * * * *  进程不在就拉起（默认 5 分钟）
# 标记行以 "# rtunnel-relay:" 开头，重复执行只替换自己的行，不动其它 crontab。
# 退出码：0 成功；2 参数错误；5 没有 crontab 命令
set -eu

PREFIX=${RTUNNEL_RELAY_PREFIX:-/opt/rtunnel/relay}
LOG=
EVERY=5
PRINT=0
REMOVE=0
TAG="# rtunnel-relay:"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --log) LOG=$2; shift 2 ;;
        --every) EVERY=$2; shift 2 ;;
        --print) PRINT=1; shift ;;
        --remove) REMOVE=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

[ -n "$LOG" ] || LOG="$PREFIX/relay.log"
RESTART="$PREFIX/restart.sh"
case "$EVERY" in ''|*[!0-9]*) echo "--every 需要正整数" >&2; exit 2 ;; esac
[ "$EVERY" -ge 1 ] || { echo "--every 需要正整数" >&2; exit 2; }

# cron 的 @reboot 行前面不能带标记注释，所以标记写在命令尾部的注释里
BOOT_LINE="@reboot $RESTART --prefix $PREFIX --log $LOG >/dev/null 2>&1 $TAG boot"
WATCH_LINE="*/$EVERY * * * * pgrep -x rtunnel-relay >/dev/null || $RESTART --prefix $PREFIX --log $LOG >/dev/null 2>&1 $TAG watch"

if [ "$PRINT" = 1 ]; then
    echo "$BOOT_LINE"
    [ "$REMOVE" = 1 ] || echo "$WATCH_LINE"
    exit 0
fi

command -v crontab >/dev/null 2>&1 || { echo "没有 crontab 命令，无法安装开机自启" >&2; exit 5; }

cur=$(mktemp) || exit 1
new=$(mktemp) || exit 1
trap 'rm -f "$cur" "$new"' EXIT
crontab -l > "$cur" 2>/dev/null || : > "$cur"

# 去掉本脚本以前写的行，保留其它条目
grep -v -F "$TAG" "$cur" > "$new" || true

if [ "$REMOVE" != 1 ]; then
    printf '%s\n' "$BOOT_LINE" >> "$new"
    printf '%s\n' "$WATCH_LINE" >> "$new"
fi

if cmp -s "$cur" "$new"; then
    echo "crontab 已是目标状态"
    exit 0
fi
crontab "$new"
if [ "$REMOVE" = 1 ]; then
    echo "已移除 rtunnel-relay 的 crontab 条目"
else
    echo "已安装 crontab: @reboot 启动 + 每 ${EVERY} 分钟存活看护"
    crontab -l | grep -F "$TAG" || true
fi
