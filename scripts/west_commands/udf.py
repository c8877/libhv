"""west udf-* 扩展命令（libhv 本地实现）。

6 个命令：udf-deps / udf-build / udf-test / udf-mp135 / udf-deploy / udf-board-test，
由仓根 west-commands.yml 注册、west.yml 的 manifest.self.west-commands 引入。

与 tuojie_dggy 的 udf.py 不同：这里不引用任何 tuojie 专有脚本/路径，只包本仓真实
存在的入口（enp_build.sh / enp_build_x86.sh / enp_build_mp135.sh / CMake / Makefile），
也不引入 Zephyr 假设（west build 是 Zephyr 的扩展，不是通用命令）。

本仓（libhv）的依赖落位约定（见 west.yml）：
  * 每个 project 的 path = third_party/<name>（相对仓根），west update 只写
    third_party/，工作树保持干净（.gitignore 里 /third_party/）；
  * udf-build 是**真自包含**的三段式：
      阶段 1  依赖就位检查：third_party/<任一依赖> 缺失 → 自动 west update
      阶段 2  依赖编译：按实测依赖序把 0 个第三方依赖装进
              third_party/prefix/<arch>/{include,lib}
              （本仓 west.yml 没有第三方依赖，阶段 2 为空）
      阶段 3  本仓构建：bash enp_build.sh --prefix <本仓前缀> --incdir <prefix>/include
              --libdir <prefix>/lib
    --no-deps 跳过阶段 2（只用已装好的前缀）。

约定：
  * 本仓没有的能力（例如库仓的板端部署）**明确失败**：打印说明 + rc=1，
    绝不假成功；
  * 子进程非 0 → 本命令以同一 rc 退出。west 的 WestCommand.run() 会忽略
    do_run() 的返回值，所以这里必须走 self.die(exit_code=rc) 才传得出去；
  * 命令行只声明本仓入口脚本**实际支持**的选项（逐个对照脚本 usage），
    不声明不存在的选项。
"""
from __future__ import annotations

import os
import platform
import re
import shlex
import shutil
import subprocess

from west.commands import WestCommand

# =============================================================================
# 本仓配置（换仓只需要改这一段）
# =============================================================================
REPO_NAME = "libhv"
BUILD_FAMILY = "enp"   # enp | xmq | cmake | make | none
BUILD_ENTRY = "enp_build.sh"     # 主机构建入口（相对仓根），"" = 没有
MP135_ENTRY = "enp_build_mp135.sh"     # MP135 交叉构建入口，"" = 没有
DEPLOY_ENTRY = ""   # 板端部署入口，"" = 本仓不适用
BOARD_TEST_ENTRY = ""   # 板端测试入口，"" = 本仓不适用
DEFAULT_BUILD_DIR = "build"         # enp_build.sh 固定把构建目录放在 <仓>/build

# 本仓自己的额外 CMake 参数（一般不填；{prefix}/{inc}/{lib} 在运行时替换成第三方前缀）。
# 只用于业务 CMakeLists 把搜索路径写死、不改源码就编不过的场合（见各仓注释）。
REPO_CMAKE_ARGS = ()

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEPS_DIR = "third_party"

# =============================================================================
# 依赖（west.yml 里 path = third_party/<name>）
#
# 依赖序 = 真实依赖链，逐条对照各依赖自己的 west.yml / CMakeLists.txt 实测得出：
#
#   本仓 west.yml 的 projects 列表为空：没有需要编译的第三方依赖，
#   阶段 2 直接跳过（仍保留三段式结构，便于以后加依赖）。
#
# 所以顺序是：本仓 west.yml 没有第三方依赖，阶段 2 为空。
# 不要按 west.yml 的书写顺序拍脑袋排——消费者必须排在提供者之后。
#
# kind:
#   "enp"   <- 依赖自带 ./enp_build.sh（支持 --prefix/--incdir/--libdir/--install）
#   "x86"   <- 只有 ./enp_build_x86.sh（位置参数=前缀，DO_INSTALL 默认 1）
#   "cmake" <- 无脚本：CMakeLists.txt 优先（cmake -S/-B + --install），
#              没有 CMakeLists.txt 时退到 make（make -j + make install PREFIX=<prefix>）
#   "autotools" <- configure/autogen.sh 工程（./configure --prefix + make + make install）
#
# 叶子依赖（没有别的依赖要链接它们）一律 --static --shared 双份：
#   本仓 CMakeLists 用裸 find_library(NAMES ...) 取库，静态库在链接行上的先后顺序
#   会决定符号解析（libyyjson.a 排在 liben_iox.a 前面 ⇒ en_iox 内的 yyjson 符号
#   解析不到）。同时产出 .so 后 find_library 选 .so，顺序无关，且 CMake 会给
#   构建树二进制自动加 RPATH，构建完就能直接跑。
DEPS = ()
# west 层自己消费、不能透传给构建入口的选项
WEST_ONLY_FLAGS = ("--no-deps",)


def _nproc() -> str:
    return str(os.cpu_count() or 4)


def _exists(rel: str) -> bool:
    return bool(rel) and os.path.isfile(os.path.join(REPO_ROOT, rel))


def _abs_dir(rel: str) -> str:
    return os.path.abspath(os.path.join(REPO_ROOT, rel or DEFAULT_BUILD_DIR))


def _arch() -> str:
    return platform.machine() or "unknown"


def _subst(arg: str, prefix: str) -> str:
    """把 {prefix}/{inc}/{lib} 替换成第三方前缀下的实际路径。"""
    return (str(arg)
            .replace("{prefix}", prefix)
            .replace("{inc}", os.path.join(prefix, "include"))
            .replace("{lib}", os.path.join(prefix, "lib")))


def _deps_prefix() -> str:
    """第三方依赖安装前缀：third_party/prefix/<arch>（UDF_DEPS_PREFIX 可覆盖）。

    构建入口（enp_build.sh 等）本身就认 UDF_DEPS_PREFIX / TUOJIE_DEPS_PREFIX；
    这里用同一组变量，保证"脚本单独跑"和"west udf-build 跑"落在同一棵前缀上。
    """
    env = os.environ.get("UDF_DEPS_PREFIX") or os.environ.get("TUOJIE_DEPS_PREFIX")
    if env:
        return os.path.abspath(env)
    return os.path.join(REPO_ROOT, DEPS_DIR, "prefix", _arch())


# =============================================================================
# 声明式选项表：(flags, kwargs) —— 只列对应入口真有的选项
# =============================================================================
_BUILD_OPTS = {
    # enp_build.sh 的完整参数集（已逐项对照各仓脚本 usage）
    "enp": (
        (("--static",), dict(action="store_true", help="只编静态库（脚本默认已是静态）")),
        (("--shared",), dict(action="store_true", help="编共享库")),
        (("--opt",), dict(choices=("none", "size", "speed"), default="size",
                          metavar="LEVEL",
                          help="体积优化级别 none|size|speed（默认 size ⇒ MinSizeRel/-Os；none 保持旧行为）")),
        (("--strip",), dict(action="store_true",
                            help="构建后 strip 产物（默认关；与 --install 组合时装 strip 后的产物）")),
        (("--examples",), dict(action="store_true", help="构建示例程序")),
        (("--tests",), dict(action="store_true", help="构建测试用例")),
        (("--with-openssl",), dict(action="store_true",
                                   help="启用 OpenSSL TLS 后端（与 --with-mbedtls/--with-gnutls 互斥）")),
        (("--with-mbedtls",), dict(action="store_true",
                                   help="启用 mbedTLS TLS 后端（与 --with-openssl/--with-gnutls 互斥）")),
        (("--with-gnutls",), dict(action="store_true",
                                  help="启用 GnuTLS TLS 后端（与 --with-openssl/--with-mbedtls 互斥）")),
        (("--install",), dict(action="store_true", help="安装到前缀")),
        (("--clean",), dict(action="store_true", help="清空构建目录后退出")),
        (("--rebuild",), dict(action="store_true", help="清空后重新编译")),
        (("--prefix",), dict(metavar="PATH", help="安装前缀（默认 .prefix/<arch>）")),
        (("--host",), dict(metavar="TRIPLET", help="交叉编译目标三元组")),
        (("--libdir",), dict(metavar="PATH", help="额外库搜索路径（冒号分隔）")),
        (("--incdir",), dict(metavar="PATH", help="额外头文件搜索路径（冒号分隔）")),
        (("--cmake-args",), dict(action="append", metavar="ARGS", help="透传 CMake 参数，可重复")),
        (("-j", "--jobs"), dict(dest="jobs", metavar="N", help="并行编译数")),
    ),
    # xmq 只有 enp_build_x86.sh：位置参数是前缀，并行/安装走环境变量
    "xmq": (
        (("--prefix",), dict(metavar="PATH", help="安装前缀（作为脚本的位置参数传入）")),
        (("-j", "--jobs"), dict(dest="jobs", metavar="N", help="并行数（导出为 JOBS）")),
        (("--no-install",), dict(dest="no_install", action="store_true",
                                 help="只编译不安装（DO_INSTALL=0）")),
    ),
    # 无 enp_build.sh 时退到 cmake -S/-B
    "cmake": (
        (("-d", "--dir"), dict(dest="dir", metavar="DIR", default=None, help="构建目录（默认 build）")),
        (("--clean",), dict(action="store_true", help="清空构建目录后重新配置")),
        (("--cmake-args",), dict(action="append", metavar="ARGS", help="透传 CMake 参数，可重复")),
        (("-j", "--jobs"), dict(dest="jobs", metavar="N", help="并行编译数")),
    ),
    # 只有 Makefile 的工程（kconfig/Makefile 项目）
    "make": (
        (("--clean",), dict(action="store_true", help="make clean 后退出")),
        (("--install",), dict(action="store_true", help="make install DESTDIR=<前缀>")),
        (("-j", "--jobs"), dict(dest="jobs", metavar="N", help="并行编译数")),
    ),
    "none": (),
}


class _RepoCommand(WestCommand):
    """本仓 udf-* 的公共基类：声明选项、起子进程、把失败传出去。"""

    OPTS = ()
    accepts_unknown_args = False

    def __init__(self):
        super().__init__(self.NAME, self.HELP, self.DESCRIPTION,
                         accepts_unknown_args=self.accepts_unknown_args)

    def do_add_parser(self, parser_adder):
        p = parser_adder.add_parser(self.NAME, help=self.HELP,
                                    description=self.DESCRIPTION)
        for flags, kw in self.OPTS:
            p.add_argument(*flags, **kw)
        return p

    @staticmethod
    def _dest(flags, kw):
        if kw.get("dest"):
            return kw["dest"]
        longs = [f for f in flags if f.startswith("--")]
        return (longs[0] if longs else flags[0]).lstrip("-").replace("-", "_")

    def _given(self, args):
        """[(flag, value|None)]，只含用户显式给出的选项，保持声明顺序。"""
        given = []
        for flags, kw in self.OPTS:
            val = getattr(args, self._dest(flags, kw), None)
            if kw.get("action") == "store_true":
                if val:
                    given.append((flags[0], None))
            elif kw.get("action") == "append":
                for item in val or []:
                    given.append((flags[0], str(item)))
            elif val not in (None, ""):
                given.append((flags[0], str(val)))
        return given

    def run_cmd(self, cmd, env=None, cwd=None):
        cmd = [str(c) for c in cmd]
        where = cwd or REPO_ROOT
        print("$ " + " ".join(cmd) + "    (cwd=" + where + ")", flush=True)
        rc = subprocess.call(cmd, cwd=where, env=env)
        if rc != 0:
            # west 忽略 do_run() 的返回值，必须 die(exit_code=rc) 才能让失败可见
            self.die("命令失败（rc=%d）：%s" % (rc, " ".join(cmd)), exit_code=rc)
        return rc

    def not_applicable(self, what, hint=""):
        print("【本仓不适用】%s：%s" % (REPO_NAME, what), flush=True)
        if hint:
            print(hint, flush=True)
        self.die("%s 不适用（rc=1）" % self.NAME, exit_code=1)


class UdfDeps(_RepoCommand):
    NAME = "udf-deps"
    HELP = "拉取本仓 manifest 声明的依赖（west update）"
    DESCRIPTION = ("等于 west update，依赖 revision 以本仓 west.yml 为准；"
                   "依赖落在 third_party/<name>（见 west.yml 的 path）。"
                   "额外参数原样透传（如 --narrow、-o=--depth=1）。")
    accepts_unknown_args = True

    def do_run(self, args, unknown):
        self.run_cmd(["west", "update"] + list(unknown))


class UdfBuild(_RepoCommand):
    NAME = "udf-build"
    HELP = "主机构建（自动 west update + 编依赖 + 编本仓）"
    DESCRIPTION = (
        "真自包含的三段式主机构建：\n"
        "  1) 依赖就位检查：third_party/<任一依赖> 缺失 → 自动执行 west update；\n"
        "  2) 依赖编译：按实测依赖序逐个编译安装到 third_party/prefix/<arch>；\n"
        "     依赖序与入口见本文件 DEPS（enp_build.sh / enp_build_x86.sh /\n"
        "     cmake -S -B --install / make 四种入口按依赖实际具备的脚本选择）；\n"
        "  3) 本仓构建：bash enp_build.sh（默认补 --prefix/--incdir/--libdir 指向前缀），其余选项原样透传。\n"
        "--no-deps 跳过第 2 段（只用已装好的前缀）；任一子进程非 0 即以同一 rc 退出。\n"
        "每段/每个依赖都打印 [udf-build] 前缀的行，失败点一眼可见。"
    )
    OPTS = _BUILD_OPTS[BUILD_FAMILY] + (
        (("--no-deps",), dict(dest="no_deps", action="store_true",
                              help="跳过依赖编译（只编本仓；要求 third_party/prefix/<arch> 已装好）")),
    )

    # ------------------------------------------------------------------ 打印
    @staticmethod
    def _stage(msg):
        print("[udf-build] " + msg, flush=True)

    # ------------------------------------------------------------ 阶段 1：就位
    def _ensure_deps(self):
        if not DEPS:
            self._stage("依赖就位检查：本仓 west.yml 没有需要编译的第三方依赖（0 个）")
            return
        missing = [d["name"] for d in DEPS
                   if not os.path.isdir(os.path.join(REPO_ROOT, DEPS_DIR, d["name"]))]
        if not missing:
            self._stage("依赖就位检查：%s/ 下 %d 个依赖齐全" % (DEPS_DIR, len(DEPS)))
            return
        self._stage("依赖就位检查：缺少 %s → 自动执行 west update"
                    % ", ".join(missing))
        self.run_cmd(["west", "update"])

    # ------------------------------------------------------------ 阶段 2：编依赖
    @staticmethod
    def _effective_kind(dep, dep_dir):
        """把声明式的 kind 解析成这套代码真正能跑的入口。

        kind = "cmake" 的依赖是"没有模型脚本的普通工程"：优先 CMakeLists.txt
        （cmake -S/-B + --install），没有 CMakeLists.txt 时退到 make（zlog 这类
        纯 Makefile 工程：make -j + make install PREFIX=<prefix>）。
        返回 "cmake" / "make" / 原 kind / None（两者都没有 ⇒ 调用方 die）。
        """
        kind = dep["kind"]
        if kind == "autotools":
            if os.path.isfile(os.path.join(dep_dir, "configure")):
                return "autotools"
            if os.path.isfile(os.path.join(dep_dir, "autogen.sh")):
                return "autotools"
            return None
        if kind not in ("cmake", "make"):
            return kind
        if os.path.isfile(os.path.join(dep_dir, "CMakeLists.txt")):
            return "cmake"
        for mk in ("Makefile", "makefile", "GNUmakefile"):
            if os.path.isfile(os.path.join(dep_dir, mk)):
                return "make"
        return None

    def _dep_cmd(self, dep, kind, prefix, jobs):
        """返回 (cmd, env)：依赖自己的构建入口 + 安装到第三方前缀。"""
        inc = os.path.join(prefix, "include")
        lib = os.path.join(prefix, "lib")

        if kind == "enp":
            # 依赖自带的 enp_build.sh：与本仓同一套参数
            cmd = ["bash", "./enp_build.sh",
                   "--prefix", prefix, "--incdir", inc, "--libdir", lib,
                   "--static", "--shared", "--install", "-j", jobs]
            for extra in dep.get("cmake_args", ()):
                cmd += ["--cmake-args", _subst(extra, prefix)]
            return cmd, None

        if kind == "x86":
            # 只有 enp_build_x86.sh：前缀是位置参数，安装/并行走环境变量
            env = os.environ.copy()
            env["DO_INSTALL"] = "1"
            env["JOBS"] = jobs
            return ["bash", "./enp_build_x86.sh", prefix], env

        raise AssertionError("_dep_cmd 不该处理 kind=%r" % kind)

    def _build_dep(self, idx, total, dep, prefix, jobs):
        name = dep["name"]
        dep_dir = os.path.join(REPO_ROOT, DEPS_DIR, name)
        self._stage("依赖编译 (%d/%d) %s：%s" % (idx, total, name, dep["why"]))
        if not os.path.isdir(dep_dir):
            self.die("依赖 %s 目录不存在（%s）：先跑 west update" % (name, dep_dir),
                     exit_code=1)

        kind = self._effective_kind(dep, dep_dir)
        if kind is None:
            self.die("依赖 %s 既没有 CMakeLists.txt 也没有 Makefile（%s），"
                     "不知道怎么编" % (name, dep_dir), exit_code=1)
        if kind == "enp" and not os.path.isfile(os.path.join(dep_dir, "enp_build.sh")):
            self.die("依赖 %s 声明为 enp 但没有 enp_build.sh（%s）" % (name, dep_dir),
                     exit_code=1)
        if kind == "x86" and not os.path.isfile(os.path.join(dep_dir, "enp_build_x86.sh")):
            self.die("依赖 %s 声明为 x86 但没有 enp_build_x86.sh（%s）" % (name, dep_dir),
                     exit_code=1)

        if kind in ("enp", "x86"):
            cmd, env = self._dep_cmd(dep, kind, prefix, jobs)
            self.run_cmd(cmd, env=env, cwd=dep_dir)

        elif kind == "make":
            # 纯 Makefile 工程（如没有 CMakeLists 的 zlog）：PREFIX 直接传进去，
            # 不碰系统 /usr/local。
            self.run_cmd(["make", "-j", jobs], cwd=dep_dir)
            self.run_cmd(["make", "install", "PREFIX=" + prefix], cwd=dep_dir)

        elif kind == "autotools":
            # autotools 工程（如 libmodbus）：configure --prefix=<前缀> + make + make install
            if not os.path.isfile(os.path.join(dep_dir, "configure")):
                self.run_cmd(["bash", "./autogen.sh"], cwd=dep_dir)
            self.run_cmd(["bash", "./configure", "--prefix", prefix], cwd=dep_dir)
            self.run_cmd(["make", "-j", jobs], cwd=dep_dir)
            self.run_cmd(["make", "install"], cwd=dep_dir)

        else:  # cmake
            build_dir = os.path.join(dep_dir, "build")
            inc = os.path.join(prefix, "include")
            lib = os.path.join(prefix, "lib")
            self.run_cmd(["cmake", "-S", ".", "-B", build_dir,
                          "-DCMAKE_BUILD_TYPE=Release",
                          "-DCMAKE_INSTALL_PREFIX=" + prefix,
                          "-DCMAKE_INCLUDE_PATH=" + inc,
                          "-DCMAKE_LIBRARY_PATH=" + lib,
                          "-DCMAKE_PREFIX_PATH=" + prefix]
                         + list(dep.get("cmake_args", ())), cwd=dep_dir)
            self.run_cmd(["cmake", "--build", build_dir, "-j", jobs], cwd=dep_dir)
            self.run_cmd(["cmake", "--install", build_dir], cwd=dep_dir)

        self._stage("依赖编译 (%d/%d) %s：完成" % (idx, total, name))

    # -------------------------------------------------------------- 阶段 3：本仓
    def _repo_args(self, args, prefix):
        """本仓构建参数：默认补 --prefix/--incdir/--libdir，其余透传（用户显式给的优先）。"""
        given = [(f, v) for (f, v) in self._given(args) if f not in WEST_ONLY_FLAGS]
        flags = set(f for f, _ in given)
        cmd = ["bash", BUILD_ENTRY]
        if "--prefix" not in flags:
            cmd += ["--prefix", os.path.join(REPO_ROOT, ".prefix", _arch())]
        if "--incdir" not in flags:
            cmd += ["--incdir", os.path.join(prefix, "include")]
        if "--libdir" not in flags:
            cmd += ["--libdir", os.path.join(prefix, "lib")]
        for extra in REPO_CMAKE_ARGS:
            cmd += ["--cmake-args", _subst(extra, prefix)]
        for flag, val in given:
            cmd.append(flag)
            if val is not None:
                cmd.append(val)
        return cmd

    def _xmq_args(self, args):
        given = dict(self._given(args))
        env = os.environ.copy()
        if given.get("--jobs"):
            env["JOBS"] = given["--jobs"]
        if "--no-install" in given:
            env["DO_INSTALL"] = "0"
        # 位置参数就是安装前缀：缺省用本仓第三方前缀（third_party/ 下，已 ignore），
        # 免得脚本在 REPO_DIR 未定义时把前缀算成 "/" 根目录（xmq 脚本 L6 的老毛病）。
        cmd = ["bash", BUILD_ENTRY, given.get("--prefix") or _deps_prefix()]
        return cmd, env

    def _cmake_build(self, args, prefix, jobs):
        if not os.path.isfile(os.path.join(REPO_ROOT, "CMakeLists.txt")):
            self.not_applicable(
                "既无 %s 也无 CMakeLists.txt，本仓不能用 cmake 构建" %
                (BUILD_ENTRY or "enp_build.sh"))
        build_dir = _abs_dir(getattr(args, "dir", None))
        if getattr(args, "clean", False) and os.path.isdir(build_dir):
            print("清理构建目录：%s" % build_dir, flush=True)
            shutil.rmtree(build_dir)
        conf = ["-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_INSTALL_PREFIX=" + os.path.join(REPO_ROOT, ".prefix", _arch()),
                "-DCMAKE_PREFIX_PATH=" + prefix,
                "-DCMAKE_INCLUDE_PATH=" + os.path.join(prefix, "include"),
                "-DCMAKE_LIBRARY_PATH=" + os.path.join(prefix, "lib")]
        for extra in getattr(args, "cmake_args", None) or []:
            conf.extend(shlex.split(extra))
        self.run_cmd(["cmake", "-S", REPO_ROOT, "-B", build_dir] + conf)
        self.run_cmd(["cmake", "--build", build_dir, "-j", jobs])
        print("构建目录：%s" % build_dir, flush=True)

    def _make_build(self, args, prefix, jobs):
        if getattr(args, "clean", False):
            self.run_cmd(["make", "clean"], cwd=REPO_ROOT)
            return
        self.run_cmd(["make", "-j", jobs], cwd=REPO_ROOT)
        if getattr(args, "install", False):
            self.run_cmd(["make", "install", "DESTDIR=" + prefix], cwd=REPO_ROOT)

    # ------------------------------------------------------------------- 入口
    def do_run(self, args, unknown):
        if unknown:
            self.die("未知参数：%s（本仓 udf-build 只声明入口脚本支持的选项，"
                     "见 west udf-build --help）" % " ".join(unknown), exit_code=2)

        if BUILD_FAMILY not in ("enp", "xmq", "cmake", "make"):
            self.not_applicable(
                "本仓没有 CMake/enp_build.sh/Makefile 构建入口（kconfig 等工程），"
                "west udf-build 不适用")

        prefix = _deps_prefix()
        jobs = getattr(args, "jobs", None) or _nproc()
        no_deps = bool(getattr(args, "no_deps", False))

        # ---- 阶段 1：依赖就位 ----
        if no_deps:
            self._stage("阶段 1/3 依赖就位检查：--no-deps，跳过 west update（前缀 %s）" % prefix)
        else:
            self._stage("阶段 1/3 依赖就位检查：%s" % prefix)
            self._ensure_deps()

        # ---- 阶段 2：编依赖 ----
        if no_deps:
            self._stage("阶段 2/3 依赖编译：--no-deps，跳过（只编本仓）")
            if DEPS and not os.path.isdir(prefix):
                self.die("--no-deps 需要已装好的第三方前缀，但 %s 不存在；"
                         "先跑不带 --no-deps 的 west udf-build" % prefix, exit_code=1)
        elif not DEPS:
            self._stage("阶段 2/3 依赖编译：本仓没有需要编译的第三方依赖（0 个），跳过")
            os.makedirs(prefix, exist_ok=True)
        else:
            self._stage("阶段 2/3 依赖编译：%d 个依赖 → %s" % (len(DEPS), prefix))
            os.makedirs(prefix, exist_ok=True)
            for i, dep in enumerate(DEPS, 1):
                self._build_dep(i, len(DEPS), dep, prefix, jobs)
            self._stage("阶段 2/3 依赖编译：%d 个全部完成" % len(DEPS))

        # ---- 阶段 3：本仓构建 ----
        self._stage("阶段 3/3 本仓构建：family=%s（第三方前缀 %s）" % (BUILD_FAMILY, prefix))
        if BUILD_FAMILY == "enp":
            self.run_cmd(self._repo_args(args, prefix))
        elif BUILD_FAMILY == "xmq":
            cmd, env = self._xmq_args(args)
            self.run_cmd(cmd, env=env)
        elif BUILD_FAMILY == "cmake":
            self._cmake_build(args, prefix, jobs)
        else:  # make
            self._make_build(args, prefix, jobs)
        self._stage("阶段 3/3 本仓构建：完成")
        self._stage("完成：本仓 build/ 与 %s" % prefix)


class UdfTest(_RepoCommand):
    NAME = "udf-test"
    HELP = "跑单元测试（ctest --test-dir <build> --output-on-failure）"
    DESCRIPTION = ("默认在 build/ 里跑 ctest；构建目录里没有测试（未开 BUILD_TESTS，"
                   "或本仓本来就没有单测）时打印“本仓无测试”并以 0 退出。"
                   "其余参数透传给 ctest（如 -R keyword、--repeat until-fail:10）。")
    accepts_unknown_args = True
    OPTS = (
        (("-d", "--dir"), dict(dest="dir", metavar="DIR", default=None,
                               help="构建目录（默认 build）")),
    )

    def do_run(self, args, unknown):
        build_dir = _abs_dir(args.dir)
        if not os.path.isdir(build_dir):
            print("构建目录不存在：%s" % build_dir, flush=True)
            self.die("udf-test 需要先构建：west udf-build", exit_code=1)

        probe = subprocess.run(["ctest", "--test-dir", build_dir, "-N"],
                               cwd=REPO_ROOT, capture_output=True, text=True)
        listing = (probe.stdout or "") + (probe.stderr or "")
        match = re.search(r"Total Tests:\s*(\d+)", listing)
        total = int(match.group(1)) if match else 0
        if "No tests were found" in listing or total == 0:
            print("本仓无测试（%s 里没有 ctest 用例）" % build_dir, flush=True)
            return

        self.run_cmd(["ctest", "--test-dir", build_dir, "--output-on-failure"]
                     + list(unknown))


class UdfMp135(_RepoCommand):
    NAME = "udf-mp135"
    HELP = "MP135 交叉构建（本仓 enp_build_mp135.sh）"
    DESCRIPTION = ("本仓 MP135 交叉构建入口：bash enp_build_mp135.sh（脚本自己 source /opt/mp135/mp135_env.sh，可选位置参数为安装前缀）。"
                   "环境缺失时脚本失败，rc 原样返回。")
    OPTS = (
        (("--prefix",), dict(metavar="PATH", default=None,
                             help="安装前缀（作为脚本的位置参数传入；给了就 --install）")),
        (("--opt",), dict(choices=("none", "size", "speed"), default="size",
                          metavar="LEVEL",
                          help="体积优化级别 none|size|speed（默认 size；原样透传给 enp_build_mp135.sh）")),
        (("--strip",), dict(action="store_true",
                            help="构建后 strip 产物（默认关；原样透传给 enp_build_mp135.sh）")),
    )

    def do_run(self, args, unknown):
        if not _exists(MP135_ENTRY):
            self.not_applicable(
                "本仓无 mp135 交叉构建入口（未找到 %s）" %
                (MP135_ENTRY or "enp_build_mp135.sh"))
        cmd = ["bash", MP135_ENTRY]
        # 位置参数仍然是安装前缀（保持原语义），其余选项原样透传
        if args.prefix:
            cmd.append(args.prefix)
        for flag, val in self._given(args):
            if flag == "--prefix":
                continue
            cmd.append(flag)
            if val is not None:
                cmd.append(val)
        self.run_cmd(cmd)


class UdfDeploy(_RepoCommand):
    NAME = "udf-deploy"
    HELP = "板端部署（本仓不适用时 rc=1）"
    DESCRIPTION = ("本仓是库，没有板端部署入口：命令打印说明并以 rc=1 退出，"
                   "避免 CI 把“不适用”当成部署成功。将来若有部署脚本，"
                   "把 udf.py 里的 DEPLOY_ENTRY 指过去即可。")
    accepts_unknown_args = True

    def do_run(self, args, unknown):
        if not _exists(DEPLOY_ENTRY):
            self.not_applicable(
                "本仓是库，没有板端部署入口（DEPLOY_ENTRY 为空）",
                "板端部署请用应用仓（如 tuojie_dggy / en_dggyocpp）的 west udf-deploy。")
        self.run_cmd(["bash", DEPLOY_ENTRY] + list(unknown))


class UdfBoardTest(_RepoCommand):
    NAME = "udf-board-test"
    HELP = "板端测试（本仓不适用时 rc=1）"
    DESCRIPTION = ("本仓是库，没有板端测试入口：命令打印说明并以 rc=1 退出。"
                   "若有 board_test 脚本，把 BOARD_TEST_ENTRY 指过去即可。")
    accepts_unknown_args = True

    def do_run(self, args, unknown):
        if not _exists(BOARD_TEST_ENTRY):
            self.not_applicable(
                "本仓是库，没有板端测试入口（BOARD_TEST_ENTRY 为空）",
                "板端回归请走应用仓，或在本仓补 scripts/board_test.sh 后指过去。")
        self.run_cmd(["bash", BOARD_TEST_ENTRY] + list(unknown))
