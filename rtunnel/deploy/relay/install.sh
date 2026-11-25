#!/bin/sh
# rtunnel-relay 安装/升级脚本（部署包内唯一入口）。
#
# 只做文件操作与 unit 的 enable/disable，绝不隐式 stop/start/restart：
#   --start    首次安装时 enable --now
#   --restart  显式重启（升级后用），并在启动失败时自动回滚一次
#
#   install.sh --pkg-dir DIR [--prefix P] [--start|--restart] [--no-units] [--force]
#   install.sh --rollback [--prefix P] [--restart]
#   install.sh --status   [--prefix P]
#
# 退出码: 0 成功, 2 参数错误, 3 权限不足, 4 无可回滚版本/缺文件,
#         5 包校验失败, 6 版本降级且未加 --force, 7 重启后健康检查失败已回滚
set -eu

PREFIX=${RTUNNEL_RELAY_PREFIX:-/opt/rtunnel/relay}
STATE_DIR=${RTUNNEL_RELAY_STATE_DIR:-}
CONF_DIR=${RTUNNEL_CONF_DIR:-}
CONF=relay.env
UNIT=rtunnel-relay.service

PKG_DIR=
ACTION=install
START=0
RESTART=0
UNITS=1
FORCE=0
# 容器/临时前缀安装时可用环境变量彻底跳过 unit 操作
[ "${RTUNNEL_INSTALL_NO_UNITS:-0}" = "1" ] && UNITS=0

usage() {
    cat <<'EOF'
用法:
  install.sh --pkg-dir DIR [--prefix P] [--start|--restart] [--no-units] [--force]
  install.sh --rollback [--prefix P] [--restart]
  install.sh --status   [--prefix P]

--start     首次安装时 enable --now 服务
--restart   显式重启服务；重启后健康检查失败会自动回滚
--no-units  只装二进制/配置，不碰 systemd（测试用）
--force     允许安装比当前更低的版本
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pkg-dir) PKG_DIR=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --start) START=1; shift ;;
        --restart) RESTART=1; shift ;;
        --no-units) UNITS=0; shift ;;
        --force) FORCE=1; shift ;;
        --rollback) ACTION=rollback; shift ;;
        --status) ACTION=status; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
done

BIN=$PREFIX/bin
STATE=$PREFIX/state

# 临时前缀（测试/容器）默认把配置和状态放在 prefix 下，避免写 /etc /var
if [ "$PREFIX" = "/opt/rtunnel/relay" ]; then
    [ -n "$CONF_DIR" ] || CONF_DIR=/etc/rtunnel
    [ -n "$STATE_DIR" ] || STATE_DIR=/var/lib/rtunnel/relay
else
    [ -n "$CONF_DIR" ] || CONF_DIR=$PREFIX/etc
    [ -n "$STATE_DIR" ] || STATE_DIR=$PREFIX/var
fi

if [ "$(id -u)" != 0 ] && [ "$PREFIX" = "/opt/rtunnel/relay" ]; then
    echo "需要 root 安装到 $PREFIX" >&2
    exit 3
fi

if [ "$ACTION" = status ]; then
    cur=$(readlink "$BIN/rtunnel-relay" 2>/dev/null || true)
    echo "prefix:   $PREFIX"
    echo "current:  ${cur:-未安装}"
    if [ -f "$STATE/previous" ]; then
        echo "previous: $(cut -f1 "$STATE/previous")"
    fi
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        echo "service:  $(systemctl is-active "$UNIT" 2>/dev/null || true)"
    fi
    exit 0
fi

# 健康检查：以 systemd 单元状态为准，未用 systemd 时跳过
restart_unit() {
    if [ "$UNITS" != 1 ] || [ ! -d /run/systemd/system ] || ! command -v systemctl >/dev/null 2>&1; then
        echo "非 systemd 环境，请手动启动: $BIN/rtunnel-relay" >&2
        return 0
    fi
    if ! systemctl restart "$UNIT"; then
        return 1
    fi
    i=0
    while [ "$i" -lt 15 ]; do
        if systemctl is-active --quiet "$UNIT"; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

do_rollback() {
    prev=$(cut -f1 "$STATE/previous" 2>/dev/null || true)
    if [ -z "$prev" ] || [ ! -f "$BIN/$prev" ]; then
        echo "回滚目标不存在: ${prev:-无}" >&2
        return 4
    fi
    tmp=$BIN/.rtunnel-relay.$$
    rm -f "$tmp"
    ln -s "$prev" "$tmp"
    mv -f "$tmp" "$BIN/rtunnel-relay"
    echo "已切换到 $prev"
    return 0
}

if [ "$ACTION" = rollback ]; then
    do_rollback || exit 4
    if [ "$RESTART" = 1 ]; then
        restart_unit || { echo "回滚后服务仍未起来，请检查 journalctl -u $UNIT" >&2; exit 7; }
    fi
    exit 0
fi

[ -n "$PKG_DIR" ] || { usage >&2; exit 2; }
[ -f "$PKG_DIR/manifest.json" ] || { echo "包内缺少 manifest.json" >&2; exit 4; }
[ -f "$PKG_DIR/bin/rtunnel-relay" ] || { echo "包内缺少 bin/rtunnel-relay" >&2; exit 4; }
[ -f "$PKG_DIR/install.sh" ] || { echo "包内缺少 install.sh" >&2; exit 4; }

# manifest 解析：只取需要的字段，不依赖 jq
json_str() {
    sed -n 's/.*"'"$1"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$PKG_DIR/manifest.json" | head -1
}
M_NAME=$(json_str name)
M_VERSION=$(json_str version)
M_ARCH=$(json_str arch)
[ "$M_NAME" = "rtunnel-relay" ] || { echo "manifest name 不是 rtunnel-relay: $M_NAME" >&2; exit 5; }
[ -n "$M_VERSION" ] && [ -n "$M_ARCH" ] || { echo "manifest 缺少 version/arch" >&2; exit 5; }

# 架构校验：交叉打包时可用 RTUNNEL_IGNORE_ARCH=1 跳过
HOST_ARCH=$(uname -m)
if [ "${RTUNNEL_IGNORE_ARCH:-0}" != 1 ] && [ "$M_ARCH" != "$HOST_ARCH" ]; then
    echo "架构不匹配: 包 $M_ARCH，本机 $HOST_ARCH" >&2
    exit 5
fi

# 包内文件校验（manifest.files 里的 sha256）
verify_files() {
    [ -f "$PKG_DIR/manifest.json" ] || return 0
    # 用 sed/grep 提取 "路径": {"sha256":"..."} 太脆，这里交给可选的 sha256sum 校验清单
    [ -f "$PKG_DIR/SHA256SUMS" ] || return 0
    ( cd "$PKG_DIR" && sha256sum -c SHA256SUMS >/dev/null ) || {
        echo "包内文件 sha256 校验失败" >&2
        return 5
    }
}
verify_files || exit 5

chmod 0755 "$PKG_DIR/bin/rtunnel-relay"

got=$("$PKG_DIR/bin/rtunnel-relay" --version 2>/dev/null | tr -d '\r\n' || true)
[ "$got" = "$M_VERSION" ] || { echo "版本自检失败: 期望 $M_VERSION，实际 ${got:-无法执行}" >&2; exit 5; }

# 拒绝降级（除非 --force）：当前版本从软链名解析
CUR_TARGET=$(readlink "$BIN/rtunnel-relay" 2>/dev/null || true)
CUR_TARGET=${CUR_TARGET##*/}
CUR_VER=${CUR_TARGET#rtunnel-relay-}
if [ "$FORCE" != 1 ] && [ -n "$CUR_VER" ] && [ "$CUR_VER" != "$M_VERSION" ]; then
    lower=$(printf '%s\n%s\n' "$CUR_VER" "$M_VERSION" | sort -V | head -1)
    if [ "$lower" = "$M_VERSION" ]; then
        echo "拒绝降级: 当前 $CUR_VER -> 目标 $M_VERSION（加 --force 可覆盖）" >&2
        exit 6
    fi
fi

mkdir -p "$BIN" "$STATE" "$PREFIX/service" "$CONF_DIR" "$STATE_DIR"

# 1. 版本化二进制：先写临时名再 rename，绝不覆盖正在执行的 inode
NEW_TARGET=rtunnel-relay-$M_VERSION
tmp=$BIN/.$NEW_TARGET.tmp.$$
cp "$PKG_DIR/bin/rtunnel-relay" "$tmp"
chmod 0755 "$tmp"
mv -f "$tmp" "$BIN/$NEW_TARGET"

# 2. 自举副本：relay 包自带 rtunnel-client，换机器/升级不再依赖 curl
if [ -f "$PKG_DIR/bin/rtunnel-client" ]; then
    cp "$PKG_DIR/bin/rtunnel-client" "$BIN/.rtunnel-client.tmp.$$"
    chmod 0755 "$BIN/.rtunnel-client.tmp.$$"
    mv -f "$BIN/.rtunnel-client.tmp.$$" "$BIN/rtunnel-client"
fi

# 3. install.sh 复制到 prefix，设备上日常 --status/--rollback 无需保留包目录
cp "$PKG_DIR/install.sh" "$PREFIX/install.sh"
chmod 0755 "$PREFIX/install.sh"
# 自举脚本副本：设备上可继续用同一个脚本升级其它组件
if [ -f "$PKG_DIR/bin/rt-bootstrap.sh" ]; then
    cp "$PKG_DIR/bin/rt-bootstrap.sh" "$PREFIX/rt-bootstrap.sh"
    chmod 0755 "$PREFIX/rt-bootstrap.sh"
fi
# 无 systemd 环境的重启脚本（有 systemd 时仍用 systemctl）
if [ -f "$PKG_DIR/bin/restart.sh" ]; then
    cp "$PKG_DIR/bin/restart.sh" "$PREFIX/restart.sh"
    chmod 0755 "$PREFIX/restart.sh"
fi

# 4. 配置：不存在才装
if [ ! -f "$CONF_DIR/$CONF" ]; then
    if [ -f "$PKG_DIR/etc/relay.args.example" ]; then
        cp "$PKG_DIR/etc/relay.args.example" "$CONF_DIR/$CONF"
    else
        printf 'RELAY_ARGS="--admin-token change-me"\n' > "$CONF_DIR/$CONF"
    fi
    chmod 0600 "$CONF_DIR/$CONF"
    echo "配置已安装: $CONF_DIR/$CONF（记得改 --admin-token）"
fi

# 5. unit：渲染 prefix/state/前端参数后安装，只 enable 不 start
if [ "$UNITS" = 1 ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if [ -f "$PKG_DIR/service/$UNIT" ]; then
        front_args=
        if [ -d "$PREFIX/web/dist" ]; then
            front_args="--admin-docroot $PREFIX/web/dist"
        fi
        sed -e "s|@PREFIX@|$PREFIX|g" \
            -e "s|@STATE_DIR@|$STATE_DIR|g" \
            -e "s|@FRONTEND_ARGS@|$front_args|g" \
            "$PKG_DIR/service/$UNIT" > "$PREFIX/service/$UNIT"
        if ! cmp -s "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"; then
            cp "$PREFIX/service/$UNIT" "/etc/systemd/system/$UNIT"
            systemctl daemon-reload >/dev/null 2>&1 || true
        fi
        systemctl enable "$UNIT" >/dev/null 2>&1 || true
    fi
    if ! id rtunnel >/dev/null 2>&1; then
        echo "提示: 系统里没有 rtunnel 用户，unit 以该用户运行；可执行 useradd -r -s /usr/sbin/nologin rtunnel" >&2
    fi
fi

# 6. 记录回滚点：软链切换才是事务提交点
if [ -n "$CUR_TARGET" ] && [ "$CUR_TARGET" != "$NEW_TARGET" ]; then
    printf '%s\t%s\n' "$CUR_TARGET" "$UNIT" > "$STATE/previous"
else
    rm -f "$STATE/previous"
fi
printf '%s\n' "$UNIT" > "$STATE/unit"

# 7. 原子切换软链
tmp=$BIN/.rtunnel-relay.$$
rm -f "$tmp"
ln -s "$NEW_TARGET" "$tmp"
mv -f "$tmp" "$BIN/rtunnel-relay"

# 8. 只保留当前与上一个
keep1=$NEW_TARGET
keep2=$(cut -f1 "$STATE/previous" 2>/dev/null || true)
for f in "$BIN"/rtunnel-relay-*; do
    [ -f "$f" ] || continue
    b=${f##*/}
    [ "$b" = "$keep1" ] && continue
    [ -n "$keep2" ] && [ "$b" = "$keep2" ] && continue
    case "$b" in .*) continue ;; esac
    rm -f "$f"
done

echo "INSTALLED=$BIN/$NEW_TARGET"

if [ "$START" = 1 ]; then
    if restart_unit; then
        echo "已启动 $UNIT"
    else
        echo "启动失败，尝试回滚" >&2
        if do_rollback; then
            restart_unit || true
            exit 7
        fi
        exit 7
    fi
elif [ "$RESTART" = 1 ]; then
    if restart_unit; then
        echo "已重启 $UNIT"
    else
        echo "重启后健康检查失败，回滚到上一版" >&2
        if do_rollback; then
            if restart_unit; then
                echo "已回滚并恢复运行" >&2
            fi
            exit 7
        fi
        exit 7
    fi
fi
