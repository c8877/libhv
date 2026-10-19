"""west udf-* 扩展命令（libhv 本地实现）。

6 个命令：udf-deps / udf-build / udf-test / udf-mp135 / udf-deploy / udf-board-test，
由仓根 west-commands.yml 注册、west.yml 的 manifest.self.west-commands 引入。

与 tuojie_dggy 的 udf.py 不同：这里不引用任何 tuojie 专有脚本/路径，只包本仓真实
存在的入口（enp_build.sh / enp_build_x86.sh / enp_build_mp135.sh / CMake），
也不引入 Zephyr 假设（west build 是 Zephyr 的扩展，不是通用命令）。

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
import re
import shlex
import shutil
import subprocess

from west.commands import WestCommand

# =============================================================================
# 本仓配置（换仓只需要改这一段）
# =============================================================================
REPO_NAME = "libhv"
BUILD_FAMILY = "enp"   # enp | xmq | cmake | none
BUILD_ENTRY = "enp_build.sh"     # 主机构建入口（相对仓根），"" = 没有
MP135_ENTRY = "enp_build_mp135.sh"     # MP135 交叉构建入口，"" = 没有
DEPLOY_ENTRY = ""   # 板端部署入口，"" = 本仓不适用
BOARD_TEST_ENTRY = ""   # 板端测试入口，"" = 本仓不适用
DEFAULT_BUILD_DIR = "build"         # enp_build.sh 固定把构建目录放在 <仓>/build

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _nproc() -> str:
    return str(os.cpu_count() or 4)


def _exists(rel: str) -> bool:
    return bool(rel) and os.path.isfile(os.path.join(REPO_ROOT, rel))


def _abs_dir(rel: str) -> str:
    return os.path.abspath(os.path.join(REPO_ROOT, rel or DEFAULT_BUILD_DIR))


# =============================================================================
# 声明式选项表：(flags, kwargs) —— 只列对应脚本 usage 里真有的选项
# =============================================================================
_BUILD_OPTS = {
    # enp_build.sh 的完整参数集（已逐项对照本仓脚本 usage）
    "enp": (
        (("--static",), dict(action="store_true", help="只编静态库（脚本默认已是静态）")),
        (("--shared",), dict(action="store_true", help="编共享库")),
        (("--examples",), dict(action="store_true", help="构建示例程序")),
        (("--tests",), dict(action="store_true", help="构建测试用例")),
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

    def run_cmd(self, cmd, env=None):
        cmd = [str(c) for c in cmd]
        print("$ " + " ".join(cmd) + "    (cwd=" + REPO_ROOT + ")", flush=True)
        rc = subprocess.call(cmd, cwd=REPO_ROOT, env=env)
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
                   "额外参数原样透传（如 --narrow、-o=--depth=1）。")
    accepts_unknown_args = True

    def do_run(self, args, unknown):
        self.run_cmd(["west", "update"] + list(unknown))


class UdfBuild(_RepoCommand):
    NAME = "udf-build"
    HELP = "主机构建（本仓 enp_build.sh）"
    DESCRIPTION = ("本仓主机构建入口：bash enp_build.sh，选项原样透传（--static/--shared/--examples/--tests/--install/--clean/--rebuild/--prefix/--host/--libdir/--incdir/--cmake-args/-j）。"
                   "只声明该入口支持的选项；子进程非 0 即以同一 rc 退出。")
    OPTS = _BUILD_OPTS[BUILD_FAMILY]

    def do_run(self, args, unknown):
        if unknown:
            self.die("未知参数：%s（本仓 udf-build 只声明入口脚本支持的选项，"
                     "见 west udf-build --help）" % " ".join(unknown), exit_code=2)

        if BUILD_FAMILY == "enp":
            cmd = ["bash", BUILD_ENTRY]
            for flag, val in self._given(args):
                cmd.append(flag)
                if val is not None:
                    cmd.append(val)
            self.run_cmd(cmd)

        elif BUILD_FAMILY == "xmq":
            given = dict(self._given(args))
            env = os.environ.copy()
            if given.get("--jobs"):
                env["JOBS"] = given["--jobs"]
            if "--no-install" in given:
                env["DO_INSTALL"] = "0"
            cmd = ["bash", BUILD_ENTRY]
            if given.get("--prefix"):
                cmd.append(given["--prefix"])
            self.run_cmd(cmd, env=env)

        elif BUILD_FAMILY == "cmake":
            self._cmake_build(args)

        else:
            self.not_applicable(
                "本仓没有 CMake/enp_build.sh 构建入口（kconfig/Makefile 工程），"
                "west udf-build 不适用")

    def _cmake_build(self, args):
        if not os.path.isfile(os.path.join(REPO_ROOT, "CMakeLists.txt")):
            self.not_applicable(
                "既无 %s 也无 CMakeLists.txt，west udf-build 不适用" %
                (BUILD_ENTRY or "enp_build.sh"))
        build_dir = _abs_dir(args.dir)
        if args.clean and os.path.isdir(build_dir):
            print("清理构建目录：%s" % build_dir, flush=True)
            shutil.rmtree(build_dir)
        conf = ["-DCMAKE_BUILD_TYPE=Release"]
        for extra in args.cmake_args or []:
            conf.extend(shlex.split(extra))
        self.run_cmd(["cmake", "-S", REPO_ROOT, "-B", build_dir] + conf)
        self.run_cmd(["cmake", "--build", build_dir, "-j", args.jobs or _nproc()])
        print("构建目录：%s" % build_dir, flush=True)


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
                   "脚本自己 source /opt/mp135/mp135_env.sh；环境缺失时脚本失败，rc 原样返回。")
    OPTS = (
        (("--prefix",), dict(metavar="PATH", default=None,
                             help="安装前缀（作为脚本的位置参数传入）")),
    )

    def do_run(self, args, unknown):
        if not _exists(MP135_ENTRY):
            self.not_applicable(
                "本仓无 mp135 交叉构建入口（未找到 %s）" %
                (MP135_ENTRY or "enp_build_mp135.sh"))
        cmd = ["bash", MP135_ENTRY]
        if args.prefix:
            cmd.append(args.prefix)
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
