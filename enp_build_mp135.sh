#!/bin/bash
# =============================================================================
# enp_build_mp135.sh - libhv 的 mp135 (STM32MP135 / arm-linux-gnueabi) 交叉编译薄包装
# 版本: 1.0.0
#
# 用法:
#   ./enp_build_mp135.sh                        # 构建（默认前缀: ~/.local/mp135）
#   ./enp_build_mp135.sh --prefix /tmp/pfx      # 构建 + 安装到 /tmp/pfx
#   ./enp_build_mp135.sh ~/.local/mp135         # 位置参数等同于 --prefix（兼容旧调用）
#   ./enp_build_mp135.sh --install ...          # 其余参数原样透传给 ./enp_build.sh
#
# 说明:
#   * 先 source mp135 交叉编译环境（默认 /opt/mp135/mp135_env.sh，可用 MP135_ENV_FILE 覆盖），
#     再从环境里的 CC 推导工具链 triple 并作为 --host 传给 enp_build.sh。
#   * 环境脚本只导出 CC/CXX 而不提供可用的 <triple>-gcc 时，退化为不传 --host，
#     改由环境变量驱动 CMake，并通过 --cmake-args 补 CMAKE_SYSTEM_NAME=Linux。
#   * 交叉构建目录隔离到 build/mp135（BUILD_DIR 可覆盖），避免与本机 x86 构建缓存串味。
#   * 安装前缀优先级: --prefix/位置参数 > UDF_DEPS_PREFIX > TUOJIE_DEPS_PREFIX > ~/.local/mp135
#   * 任一步失败即非 0 退出。
#
# 示例:
#   ./enp_build_mp135.sh --prefix /tmp/libhv-mp135
#   UDF_DEPS_PREFIX=/opt/deps/mp135 ./enp_build_mp135.sh
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="${MP135_ENV_FILE:-/opt/mp135/mp135_env.sh}"

die() {
    echo "错误: $*" >&2
    exit 1
}

[ -r "${SCRIPT_DIR}/enp_build.sh" ] || die "未找到 ${SCRIPT_DIR}/enp_build.sh（本脚本要求与 enp_build.sh 同目录）"

# 1) 导入 mp135 交叉编译环境
[ -r "${ENV_FILE}" ] || die "未找到 mp135 交叉编译环境脚本: ${ENV_FILE}
请先安装 MP135 SDK（应存在 /opt/mp135/mp135_env.sh），
或用 MP135_ENV_FILE=<path> 指定环境脚本路径。"
# shellcheck source=/dev/null
. "${ENV_FILE}"

# 2) 解析默认安装前缀
[ -n "${HOME:-}" ] || HOME=~
PREFIX="${UDF_DEPS_PREFIX:-${TUOJIE_DEPS_PREFIX:-${HOME}/.local/mp135}}"

# 3) 透传参数；--prefix / 首个位置参数覆盖默认前缀
PASS_ARGS=()
SPECIFIED_PREFIX=0
DO_INSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || die "选项 --prefix 需要一个参数"
            PREFIX="$2"
            SPECIFIED_PREFIX=1
            PASS_ARGS+=("$1" "$2")
            shift 2
            ;;
        --install)
            DO_INSTALL=1
            PASS_ARGS+=("$1")
            shift
            ;;
        -h|--help)
            sed -n '/^# 用法:/,/^# 示例:/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            PASS_ARGS+=("$1")
            shift
            ;;
        *)
            # 兼容旧约定: 第一个位置参数即安装前缀
            if [ ${SPECIFIED_PREFIX} -eq 0 ]; then
                PREFIX="$1"
                SPECIFIED_PREFIX=1
                PASS_ARGS+=("--prefix" "$1")
            else
                PASS_ARGS+=("$1")
            fi
            shift
            ;;
    esac
done

# 显式给了前缀就默认执行安装（与 Makefile mp135_install 的既有语义一致）
if [ ${SPECIFIED_PREFIX} -eq 1 ]; then
    [ ${DO_INSTALL} -eq 1 ] || { PASS_ARGS+=(--install); DO_INSTALL=1; }
fi

mkdir -p "${PREFIX}" || die "无法创建安装前缀目录: ${PREFIX}"
PREFIX="$(cd "${PREFIX}" && pwd)"

# 4) 交叉工具链: 从 CC 推导 triple，可用时走 --host
TRIPLE=""
if [ -n "${CC:-}" ]; then
    _cc_bin="${CC%% *}"
    _cc_base="$(basename "${_cc_bin}")"
    case "${_cc_base}" in
        *-gcc) TRIPLE="${_cc_base%-gcc}" ;;
    esac
fi

HOST_ARGS=()
if [ -n "${TRIPLE}" ] \
   && command -v "${TRIPLE}-gcc" >/dev/null 2>&1 \
   && command -v "${TRIPLE}-g++" >/dev/null 2>&1; then
    HOST_ARGS=(--host "${TRIPLE}")
    echo "[enp_build_mp135] 交叉工具链: ${TRIPLE} (CC=${CC:-未设置})"
else
    # 环境只给了 CC/CXX 或 triple 不完整: 不传 --host，靠环境 + CMake 参数交叉
    echo "[enp_build_mp135] 警告: 未找到可用的 <triple>-gcc/g++（CC=${CC:-未设置}），改用环境变量驱动 CMake"
    HOST_ARGS=(--cmake-args "-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm")
    [ -n "${CC:-}" ] && HOST_ARGS+=(--cmake-args "-DCMAKE_C_COMPILER=${_cc_bin}")
    [ -n "${CXX:-}" ] && HOST_ARGS+=(--cmake-args "-DCMAKE_CXX_COMPILER=${CXX%% *}")
fi

# 5) 交叉构建目录隔离（避免复用本机 x86 的 CMakeCache）
export BUILD_DIR="${BUILD_DIR:-build/mp135}"

echo "[enp_build_mp135] 安装前缀: ${PREFIX}"
echo "[enp_build_mp135] 构建目录: ${BUILD_DIR}"
echo "[enp_build_mp135] 环境脚本: ${ENV_FILE}"

# 6) 转调 enp_build.sh（默认静态库 + 共享库）
set +e
"${SCRIPT_DIR}/enp_build.sh" --static --shared "${HOST_ARGS[@]}" --prefix "${PREFIX}" "${PASS_ARGS[@]}"
rc=$?
set -e

if [ ${rc} -ne 0 ]; then
    echo "[enp_build_mp135] 失败: enp_build.sh 返回 ${rc}" >&2
    exit "${rc}"
fi

echo "[enp_build_mp135] 完成: ${PREFIX}"
