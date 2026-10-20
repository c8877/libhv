#!/bin/bash
# =============================================================================
# enp_build.sh - libhv 一键编译脚本
# 版本: 1.0.0
#
# 用法:
#   ./enp_build.sh [选项]
#
# 选项:
#   --prefix <path>       安装前缀 (CMAKE_INSTALL_PREFIX，默认: <仓库父目录>/.prefix/libhv-<arch>)
#   --static              编译静态库 (libhv_static)       默认: 开
#   --shared              编译共享库 (hv)                 默认: 开
#   --opt <level>         体积优化级别: none|size|speed (默认: size ⇒ MinSizeRel/-Os)
#   --strip               构建后 strip 产物 (默认: 关；与 --install 组合时装 strip 后的产物)
#   --examples            构建 examples/ (BUILD_EXAMPLES) 默认: 关
#   --tests               构建 unittest/ (BUILD_UNITTEST) 默认: 关
#   --benchs              构建基准测试 (libhv 的 CMake 无此开关，忽略并打印说明)
#   --install             构建后执行 cmake --install 装到 --prefix
#   --incdir <p[:p2]>     额外头文件搜索路径 (追加到 CMAKE_INCLUDE_PATH，并 -I 进 C/CXX_FLAGS)
#   --libdir <p[:p2]>     额外库搜索路径   (追加到 CMAKE_LIBRARY_PATH，并 -L 进链接参数)
#   --cmake-args <args>   自定义 CMake 参数，原样透传，可多次使用
#   --host <triple>       交叉编译前缀 (校验 <triple>-gcc/g++ 存在后设 CMAKE_C_COMPILER 等)
#   --jobs, -j <N>        并行编译数 (默认: nproc)
#   --clean               清理构建目录后退出
#   --rebuild             清理构建目录后重新编译
#   --help, -h            显示帮助
#
# 环境变量:
#   BUILD_DIR             构建目录 (默认: <仓库根>/build；相对路径按仓库根解析)
#                         例: BUILD_DIR=build/x86_64 ./enp_build.sh --static
#   CMAKE_INCLUDE_PATH / CMAKE_LIBRARY_PATH / CMAKE_PREFIX_PATH
#                         传入时与 --incdir/--libdir/--prefix 以 ; 追加拼接，不覆盖
#   CMAKE_BUILD_TYPE      默认 MinSizeRel（由 --opt 决定：size=MinSizeRel / speed=Release / none=不传）
#
# 说明:
#   * 源码根即仓库根，CMakeLists.txt 在根目录；脚本可在任意目录被调用（按脚本所在目录定位源码）。
#   * libhv 的 CMakeLists 在 configure 阶段会往源码树写 hconfig.h 与 include/hv/（上游行为，
#     include/ 与 build/ 已被 .gitignore 忽略；hconfig.h 是跟踪文件，配置后 git status 可能显示其改动）。
#   * 任一 cmake 配置/构建/安装失败都会打印 [enp_build] ... failed 并 exit 1。
#
# 示例:
#   ./enp_build.sh --static --shared --examples --install --prefix /tmp/libhv-pfx
#   BUILD_DIR=build/x86_64 ./enp_build.sh --static --shared -j 8
#   ./enp_build.sh --shared --cmake-args "-DWITH_OPENSSL=ON" --cmake-args "-DWITH_MQTT=ON"
# =============================================================================

set -e

SCRIPT_VERSION="1.0.0"

# 以脚本所在目录作为源码根（与执行时的 PWD 无关）
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "错误: 在 ${PROJECT_DIR} 中未找到 CMakeLists.txt（本脚本要求与源码根同级）" >&2
    exit 1
fi

PROJECT_NAME="libhv"

# -----------------------------------------------------------------------------
# 默认配置
# -----------------------------------------------------------------------------
ARCH="$(uname -m)"
DEFAULT_BUILD_DIR="${PROJECT_DIR}/build"
# BUILD_DIR 环境变量可覆盖构建目录；相对路径按仓库根解析，避免污染源码树
if [ -n "${BUILD_DIR:-}" ]; then
    case "$BUILD_DIR" in
        /*) : ;;
        *) BUILD_DIR="${PROJECT_DIR}/${BUILD_DIR}" ;;
    esac
else
    BUILD_DIR="$DEFAULT_BUILD_DIR"
fi

PREFIX="${PREFIX:-${TUOJIE_DEPS_PREFIX:-$(cd "$PROJECT_DIR/.." 2>/dev/null && pwd)/.prefix/libhv-${ARCH}}}"
HOST=""
INCDIR=""
LIBDIR=""
BUILD_STATIC=ON
BUILD_SHARED=ON
BUILD_EXAMPLES=OFF
BUILD_TESTS=OFF
DO_INSTALL=0
CLEAN=0
REBUILD=0
NPROC=""
CMAKE_EXTRA_ARGS=()
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
# 体积优化级别：none(不传 CMAKE_BUILD_TYPE，与改动前完全一致) / size(MinSizeRel ⇒ -Os -DNDEBUG) /
# speed(Release ⇒ -O3 -DNDEBUG)。默认 size —— 用户要的是"优化体积"。
OPT_LEVEL="size"
# 构建后是否 strip：默认关，避免调试流程（gdb/addr2line）被误 strip 破坏。
DO_STRIP=0

SPECIFIED_STATIC=0
SPECIFIED_SHARED=0
SPECIFIED_PREFIX=0
SPECIFIED_LIBDIR=0
SPECIFIED_INCDIR=0

usage() {
    # 直接打印本文件头部的 "用法/选项/环境变量/说明" 注释段，避免帮助文本与实现脱节
    sed -n '/^# 用法:/,/^# 示例:/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'
    exit 0
}

die() {
    echo "错误: $*" >&2
    exit 1
}

need_arg() {
    # $1 = 选项名, $2 = 剩余参数个数
    [ "$2" -ge 2 ] || die "选项 $1 需要一个参数"
}

# 把 : 或 , 分隔的路径列表规范化成 ; 分隔，并与已有值追加拼接（不覆盖）
append_paths() {
    local existing="$1" add="$2"
    local norm="${add//:/;}"
    norm="${norm//,/;}"
    while [ "${norm}" != "${norm//;;/;}" ]; do norm="${norm//;;/;}"; done
    norm="${norm#;}"
    norm="${norm%;}"
    if [ -z "$existing" ]; then
        printf '%s' "$norm"
    elif [ -z "$norm" ]; then
        printf '%s' "$existing"
    else
        printf '%s;%s' "$existing" "$norm"
    fi
}

# -----------------------------------------------------------------------------
# 参数解析
# -----------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            need_arg "$1" $#
            PREFIX="$2"
            SPECIFIED_PREFIX=1
            shift 2
            ;;
        --host)
            need_arg "$1" $#
            HOST="$2"
            shift 2
            ;;
        --libdir)
            need_arg "$1" $#
            LIBDIR="$2"
            SPECIFIED_LIBDIR=1
            shift 2
            ;;
        --incdir)
            need_arg "$1" $#
            INCDIR="$2"
            SPECIFIED_INCDIR=1
            shift 2
            ;;
        --cmake-args)
            need_arg "$1" $#
            # 支持一次传多个参数（"-DX=1 -DY=2"）或多次使用
            read -r -a _extra_args <<< "$2"
            CMAKE_EXTRA_ARGS+=("${_extra_args[@]}")
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --rebuild)
            REBUILD=1
            shift
            ;;
        --static)
            BUILD_STATIC=ON
            SPECIFIED_STATIC=1
            shift
            ;;
        --shared)
            BUILD_SHARED=ON
            SPECIFIED_SHARED=1
            shift
            ;;
        --no-static)
            BUILD_STATIC=OFF
            SPECIFIED_STATIC=1
            shift
            ;;
        --no-shared)
            BUILD_SHARED=OFF
            SPECIFIED_SHARED=1
            shift
            ;;
        --opt)
            need_arg "$1" $#
            OPT_LEVEL="$2"
            shift 2
            ;;
        --opt=*)
            OPT_LEVEL="${1#--opt=}"
            shift
            ;;
        --strip)
            DO_STRIP=1
            shift
            ;;
        --jobs|-j)
            need_arg "$1" $#
            NPROC="$2"
            shift 2
            ;;
        --examples)
            BUILD_EXAMPLES=ON
            shift
            ;;
        --tests)
            BUILD_TESTS=ON
            shift
            ;;
        --benchs)
            # libhv 的 CMakeLists 没有 benchs 开关（examples/ 里已含 wrk 等压测工具）
            echo "[enp_build] 提示: libhv 的 CMake 工程无 --benchs 对应开关，已忽略（examples/ 已含 wrk 等压测程序）"
            shift
            ;;
        --install)
            DO_INSTALL=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "错误: 未知选项 '$1'" >&2
            usage
            ;;
    esac
done

# 校验 --opt 取值（--opt= 空值也会落到这里）
case "$OPT_LEVEL" in
    none|size|speed) ;;
    *)
        echo "错误: --opt 只接受 none|size|speed（当前: '$OPT_LEVEL'）" >&2
        exit 1
        ;;
esac

if [ "$BUILD_STATIC" = "OFF" ] && [ "$BUILD_SHARED" = "OFF" ]; then
    die "至少需要启用一种库类型（--static 或 --shared）"
fi

# prefix 转绝对路径（相对路径基于调用目录）
mkdir -p "$PREFIX" || die "无法创建安装前缀目录: $PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

# 未显式指定 --libdir/--incdir 时，跟随 prefix 下已存在的 lib/include
# （刚创建的空 prefix 目录不追加，避免无谓的 "目录不存在" 警告；CMAKE_PREFIX_PATH 已覆盖查找）
if [ $SPECIFIED_PREFIX -eq 1 ]; then
    [ $SPECIFIED_LIBDIR -eq 0 ] && [ -d "${PREFIX}/lib" ] && LIBDIR="${PREFIX}/lib"
    [ $SPECIFIED_INCDIR -eq 0 ] && [ -d "${PREFIX}/include" ] && INCDIR="${PREFIX}/include"
fi

echo "=============================================================="
echo "enp_build.sh v${SCRIPT_VERSION}  (${PROJECT_NAME})"
echo "=============================================================="
echo "用法: $0 [--static] [--shared] [--examples] [--install] [--prefix <dir>] [-j <N>]"
echo ""
echo "系统架构: $ARCH"
echo "源码目录: $PROJECT_DIR"
echo "构建目录: $BUILD_DIR$([ "$BUILD_DIR" = "$DEFAULT_BUILD_DIR" ] && echo " (默认)" || echo " (BUILD_DIR 覆盖)")"
echo "安装前缀: $PREFIX"
echo "构建类型: $BUILD_TYPE"
case "$OPT_LEVEL" in
    size)  echo "优化级别: size (MinSizeRel: -Os -DNDEBUG)" ;;
    speed) echo "优化级别: speed (Release: -O3 -DNDEBUG)" ;;
    none)  echo "优化级别: none (不传 CMAKE_BUILD_TYPE)" ;;
esac
echo "Strip 产物: $DO_STRIP"
[ -n "$HOST" ] && echo "交叉编译: $HOST"
[ -n "$LIBDIR" ] && echo "库路径  : $LIBDIR"
[ -n "$INCDIR" ] && echo "头文件  : $INCDIR"
echo "库类型  : 静态库($BUILD_STATIC), 共享库($BUILD_SHARED)"
echo "示例程序: $BUILD_EXAMPLES"
echo "单元测试: $BUILD_TESTS"
echo "执行安装: $([ "$DO_INSTALL" -eq 1 ] && echo "是" || echo "否")"
[ ${#CMAKE_EXTRA_ARGS[@]} -gt 0 ] && echo "自定义 CMake 参数: ${CMAKE_EXTRA_ARGS[*]}"
echo ""

if [ $CLEAN -eq 1 ] || [ $REBUILD -eq 1 ]; then
    echo "清理构建目录: $BUILD_DIR"
    rm -rf "$BUILD_DIR" || die "清理构建目录失败: $BUILD_DIR"
    if [ $CLEAN -eq 1 ]; then
        echo "=== 清理完成 ==="
        exit 0
    fi
fi

mkdir -p "$BUILD_DIR" || die "无法创建构建目录: $BUILD_DIR"
cd "$BUILD_DIR"

# -----------------------------------------------------------------------------
# 组装 CMake 参数
# -----------------------------------------------------------------------------
CMAKE_ARGS=(
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DBUILD_STATIC="$BUILD_STATIC"
    -DBUILD_SHARED="$BUILD_SHARED"
    -DBUILD_EXAMPLES="$BUILD_EXAMPLES"
)

# --examples 的开关在 libhv 根 CMakeLists 里确实存在（option(BUILD_EXAMPLES ...)），直接映射
if ! grep -q "BUILD_EXAMPLES" "$PROJECT_DIR/CMakeLists.txt" 2>/dev/null; then
    echo "[enp_build] 提示: CMakeLists.txt 未声明 BUILD_EXAMPLES，--examples 被忽略"
fi

# --tests -> BUILD_UNITTEST（libhv 的命名），未知则忽略
if grep -q "BUILD_UNITTEST" "$PROJECT_DIR/CMakeLists.txt" 2>/dev/null; then
    CMAKE_ARGS+=(-DBUILD_UNITTEST="$BUILD_TESTS")
elif [ "$BUILD_TESTS" = "ON" ]; then
    echo "[enp_build] 提示: CMakeLists.txt 无 BUILD_UNITTEST 开关，--tests 被忽略"
fi

# prefix 同时进 CMAKE_PREFIX_PATH（与已有值 ; 拼接，不覆盖）
CMAKE_PREFIX_PATH_VAL="$(append_paths "${CMAKE_PREFIX_PATH:-}" "$PREFIX")"
CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH_VAL")

# --incdir: 追加到 CMAKE_INCLUDE_PATH，并注入 C/CXX_FLAGS 保证真的被用到
if [ -n "$INCDIR" ]; then
    CMAKE_INCLUDE_PATH_VAL="$(append_paths "${CMAKE_INCLUDE_PATH:-}" "$INCDIR")"
    CMAKE_ARGS+=(-DCMAKE_INCLUDE_PATH="$CMAKE_INCLUDE_PATH_VAL")
    IFS=';' read -r -a _inc_paths <<< "$INCDIR"
    _inc_flags=""
    for _p in "${_inc_paths[@]}"; do
        [ -z "$_p" ] && continue
        [ -d "$_p" ] || echo "[enp_build] 警告: 头文件目录不存在: $_p"
        _inc_flags="$_inc_flags -I$_p"
    done
    CMAKE_ARGS+=(-DCMAKE_C_FLAGS="${CMAKE_C_FLAGS:-}$_inc_flags")
    CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS:-}$_inc_flags")
fi

# --libdir: 追加到 CMAKE_LIBRARY_PATH，并注入 -L 到可执行/共享库链接参数
if [ -n "$LIBDIR" ]; then
    CMAKE_LIBRARY_PATH_VAL="$(append_paths "${CMAKE_LIBRARY_PATH:-}" "$LIBDIR")"
    CMAKE_ARGS+=(-DCMAKE_LIBRARY_PATH="$CMAKE_LIBRARY_PATH_VAL")
    IFS=';' read -r -a _lib_paths <<< "$LIBDIR"
    _lib_flags=""
    for _p in "${_lib_paths[@]}"; do
        [ -z "$_p" ] && continue
        [ -d "$_p" ] || echo "[enp_build] 警告: 库目录不存在: $_p"
        _lib_flags="$_lib_flags -L$_p"
    done
    CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="${CMAKE_EXE_LINKER_FLAGS:-}$_lib_flags")
    CMAKE_ARGS+=(-DCMAKE_SHARED_LINKER_FLAGS="${CMAKE_SHARED_LINKER_FLAGS:-}$_lib_flags")
fi

# --host: 交叉编译工具链
if [ -n "$HOST" ]; then
    command -v "${HOST}-gcc" >/dev/null 2>&1 || die "找不到交叉编译器 ${HOST}-gcc"
    command -v "${HOST}-g++" >/dev/null 2>&1 || die "找不到交叉编译器 ${HOST}-g++"
    CMAKE_ARGS+=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=arm
        -DCMAKE_C_COMPILER="${HOST}-gcc"
        -DCMAKE_CXX_COMPILER="${HOST}-g++"
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
    )
fi

# 体积/速度优化（CMAKE_BUILD_TYPE）
# none ⇒ 不传 CMAKE_BUILD_TYPE（与改动前一致，CMake 用自己的默认值）
case "$OPT_LEVEL" in
    size)
        CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=MinSizeRel)
        echo "优化参数: -DCMAKE_BUILD_TYPE=MinSizeRel"
        ;;
    speed)
        CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=Release)
        echo "优化参数: -DCMAKE_BUILD_TYPE=Release"
        ;;
    none)
        echo "优化参数: 无（不传 CMAKE_BUILD_TYPE，保持改动前行为）"
        ;;
esac

# --cmake-args: 原样透传，放在最后以便覆盖上面的默认值
if [ ${#CMAKE_EXTRA_ARGS[@]} -gt 0 ]; then
    CMAKE_ARGS+=("${CMAKE_EXTRA_ARGS[@]}")
fi

echo "CMake 参数: ${CMAKE_ARGS[*]}"
echo ""

# -----------------------------------------------------------------------------
# 配置 / 构建 / 安装（任一步失败即非 0 退出）
# -----------------------------------------------------------------------------
echo "配置 CMake..."
cmake "$PROJECT_DIR" "${CMAKE_ARGS[@]}" \
    || { echo "[enp_build] cmake configure failed (source=$PROJECT_DIR, build=$BUILD_DIR)" >&2; exit 1; }

echo ""
echo "编译..."

if [ -z "$NPROC" ]; then
    NPROC="$(nproc 2>/dev/null || echo 4)"
fi
case "$NPROC" in
    ''|*[!0-9]*) die "-j/--jobs 需要正整数，当前: $NPROC" ;;
    0) die "-j/--jobs 需要正整数，当前: 0" ;;
esac

echo "并行数: $NPROC"
cmake --build . -j "$NPROC" \
    || { echo "[enp_build] cmake build failed (build=$BUILD_DIR)" >&2; exit 1; }

# =============================================================================
# strip（可选，构建后 / 安装前 ⇒ --install 装的是 strip 后的产物）
# =============================================================================

# 选 strip 工具：--host <triplet> ⇒ <triplet>-strip；STRIP 环境变量；CC 推导；本地 strip
pick_strip_tool() {
    if [ -n "$HOST" ]; then
        echo "${HOST}-strip"
        return 0
    fi
    if [ -n "${STRIP:-}" ]; then
        echo "${STRIP}"
        return 0
    fi
    if [ -n "${CC:-}" ]; then
        local cc_base
        cc_base=$(basename "${CC}")
        case "$cc_base" in
            *g++)    echo "${cc_base%g++}strip" ;;
            *gcc)    echo "${cc_base%gcc}strip" ;;
            *clang++) echo "${cc_base%clang++}strip" ;;
            *clang)  echo "${cc_base%clang}strip" ;;
            *cc)     echo "${cc_base%cc}strip" ;;
            *)       echo "strip" ;;
        esac
        return 0
    fi
    echo "strip"
}

fmt_bytes() {
    awk -v b="$1" 'BEGIN {
        if (b >= 1048576) printf "%.2f MB", b / 1048576;
        else if (b >= 1024) printf "%.1f KB", b / 1024;
        else printf "%d B", b;
    }'
}

if [ "$DO_STRIP" -eq 1 ]; then
    echo ""
    echo "=== strip 产物（构建目录，安装前）==="
    STRIP_TOOL=$(pick_strip_tool)
    if ! command -v "$STRIP_TOOL" >/dev/null 2>&1; then
        echo "警告: 未找到 strip 工具 '$STRIP_TOOL'，跳过 strip（产物保留调试信息，构建继续）"
    else
        echo "strip 工具: $(command -v "$STRIP_TOOL")"
        STRIP_TOTAL_BEFORE=0
        STRIP_TOTAL_AFTER=0
        STRIP_OK=0
        STRIP_SKIP=0

        strip_one() {
            local file="$1" mode="$2" rel before after rc err
            rel="${file#"$BUILD_DIR"/}"
            before=$(stat -c '%s' "$file" 2>/dev/null || echo 0)
            rc=0
            err=$("$STRIP_TOOL" $mode "$file" 2>&1) || rc=$?
            if [ $rc -ne 0 ]; then
                echo "  警告: strip 失败（rc=$rc），已跳过: $rel"
                [ -n "$err" ] && echo "        $err"
                STRIP_SKIP=$((STRIP_SKIP + 1))
                return 0
            fi
            after=$(stat -c '%s' "$file" 2>/dev/null || echo 0)
            if [ "$before" -gt 0 ]; then
                echo "  $rel: $before → $after 字节（$(fmt_bytes "$before") → $(fmt_bytes "$after")，$(awk -v b="$before" -v a="$after" 'BEGIN { printf "%+.1f%%", (a - b) * 100.0 / b }')）"
            else
                echo "  $rel: $before → $after 字节"
            fi
            STRIP_TOTAL_BEFORE=$((STRIP_TOTAL_BEFORE + before))
            STRIP_TOTAL_AFTER=$((STRIP_TOTAL_AFTER + after))
            STRIP_OK=$((STRIP_OK + 1))
        }

        while IFS= read -r -d '' f; do
            strip_one "$f" "--strip-unneeded"
        done < <(find "$BUILD_DIR" -type f \( -name '*.so' -o -name '*.so.*' \) -print0 2>/dev/null)

        while IFS= read -r -d '' f; do
            strip_one "$f" "-g"
        done < <(find "$BUILD_DIR" -type f -name '*.a' -print0 2>/dev/null)

        while IFS= read -r -d '' f; do
            strip_one "$f" "--strip-unneeded"
        done < <(find "$BUILD_DIR" -type f -executable \
                    ! -name '*.so' ! -name '*.so.*' ! -name '*.a' \
                    ! -path '*/CMakeFiles/*' -print0 2>/dev/null)

        echo "strip 完成: 成功 $STRIP_OK 个，跳过 $STRIP_SKIP 个"
        if [ "$STRIP_OK" -gt 0 ]; then
            echo "strip 合计: $STRIP_TOTAL_BEFORE → $STRIP_TOTAL_AFTER 字节（$(fmt_bytes "$STRIP_TOTAL_BEFORE") → $(fmt_bytes "$STRIP_TOTAL_AFTER")）"
        fi
    fi
fi


if [ $DO_INSTALL -eq 1 ]; then
    echo ""
    echo "安装到 $PREFIX ..."
    cmake --install . \
        || { echo "[enp_build] cmake install failed (prefix=$PREFIX)" >&2; exit 1; }
fi

echo ""
echo "=== 构建完成 ==="
echo "项目    : $PROJECT_NAME"
echo "源码目录: $PROJECT_DIR"
echo "构建目录: $BUILD_DIR"
echo "库类型  : $([ "$BUILD_STATIC" = "ON" ] && echo "静态库 (.a)")$([ "$BUILD_STATIC" = "ON" ] && [ "$BUILD_SHARED" = "ON" ] && echo ", ")$([ "$BUILD_SHARED" = "ON" ] && echo "共享库 (.so)")"
if [ -d "$BUILD_DIR/lib" ]; then
    echo "构建产物:"
    ls -1 "$BUILD_DIR/lib" 2>/dev/null | sed 's/^/  lib\//'
fi
if [ $DO_INSTALL -eq 1 ]; then
    echo "安装前缀: $PREFIX"
    [ -d "$PREFIX/lib" ] && ls -1 "$PREFIX/lib" 2>/dev/null | sed 's/^/  lib\//'
    [ -d "$PREFIX/include/hv" ] && echo "  include/hv/ ($(ls -1 "$PREFIX/include/hv" 2>/dev/null | wc -l) 个头文件)"
fi
echo ""
