#!/usr/bin/env python3
"""部署包自举 e2e：打包 → 发布到文件仓库 → 用 rtunnel-client 下载 → 安装 → 回滚。

不依赖 cmake：用 build/bin 下已构建的二进制组装一个最小 deploy 目录，
再跑 deploy/ 里的脚本，覆盖的是脚本与包格式本身。

场景：
  1. build_packages.sh / package_agent.sh 产出统一布局的 tar.gz + .sha256
  2. publish.sh 上传到 packages/<c>/<ver>/<arch>/ 并写 latest 指针
  3. publish 重复执行 → 全部 skip（幂等）
  4. rt-bootstrap.sh 用 client 下载 latest 并安装到临时 prefix
  5. 包被篡改 → bootstrap 校验失败（退出码 5），不切换软链
  6. install.sh --rollback 切回上一版
"""

import os
import shutil
import stat
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-packages"
VER = subprocess.check_output([os.path.join(BIN, "rtunnel-relay"), "--version"],
                              env=dict(os.environ, LD_LIBRARY_PATH=LIB)).decode().strip()
DEPLOY = os.path.join(LOG, "deploy")
OUT = os.path.join(DEPLOY, "packages")


def env_with(path):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    env["PATH"] = path + ":" + env.get("PATH", "")
    return env


def run(cmd, env, timeout=180):
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def prepare_deploy():
    for sub in ("relay/bin", "agent/bin"):
        os.makedirs(os.path.join(DEPLOY, sub), exist_ok=True)
    for src, dst in (
        ("rtunnel-relay", "relay/bin/rtunnel-relay"),
        ("rtunnel-client", "agent/bin/rtunnel-client"),
        ("rtunnel-agent", "agent/bin/rtunnel-agent"),
    ):
        shutil.copy2(os.path.join(BIN, src), os.path.join(DEPLOY, dst))


def create_atk(base):
    _, call = e2e.admin_client(base)
    return call("POST", "/api/v1/admin-tokens", {"name": "pkg-test"})["token"]


def main():
    os.makedirs(LOG, exist_ok=True)
    for name in ("admin.db", "admin.db-wal", "admin.db-shm"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass
    for d in ("files", "deploy"):
        p = os.path.join(LOG, d)
        if os.path.isdir(p):
            shutil.rmtree(p)
        os.makedirs(p, exist_ok=True)

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port
    env = env_with(BIN)
    log = open(os.path.join(LOG, "relay.log"), "w")
    relay = subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
         "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "admintok",
         "--admin-db", os.path.join(LOG, "admin.db"),
         "--admin-file-dir", os.path.join(LOG, "files")],
        stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        time.sleep(0.8)
        atk = create_atk(base)
        atk_file = os.path.join(LOG, "atk")
        with open(atk_file, "w") as f:
            f.write(atk + "\n")
        os.chmod(atk_file, stat.S_IRUSR | stat.S_IWUSR)
        env["RT"] = base
        env["ATK_FILE"] = atk_file
        env["RTUNNEL_ARCH"] = "x86_64"
        env["RTUNNEL_DEPLOY_DIR"] = DEPLOY
        env["RTUNNEL_CLIENT"] = os.path.join(BIN, "rtunnel-client")

        prepare_deploy()
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/build_packages.sh"),
                       "--out", OUT], env)
        e2e.check("pkg: relay/client 打包成功", rc == 0, out[-400:])
        for f in ("rtunnel-relay", "rtunnel-client"):
            name = "%s-%s-x86_64.tar.gz" % (f, VER)
            e2e.check("pkg: 生成 %s" % name, os.path.isfile(os.path.join(OUT, name)), "")

        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/package_agent.sh")], env)
        e2e.check("pkg: agent 打包成功", rc == 0, out[-300:])
        e2e.check("pkg: 生成 agent lite 包",
                  os.path.isfile(os.path.join(OUT, "rtunnel-agent-lite-%s-x86_64.tar.gz" % VER)), "")

        # 目录结构符合约定
        rc, out = run(["tar", "-tzf", os.path.join(OUT, "rtunnel-relay-%s-x86_64.tar.gz" % VER)], env)
        e2e.check("pkg: relay 包为顶层目录",
                  out.splitlines()[0].startswith("rtunnel-relay-%s-x86_64/" % VER), out[:120])
        for part in ("bin/rtunnel-relay", "bin/rtunnel-client", "bin/rt-bootstrap.sh",
                     "install.sh", "manifest.json", "SHA256SUMS",
                     "service/rtunnel-relay.service"):
            e2e.check("pkg: relay 包含 %s" % part,
                      ("rtunnel-relay-%s-x86_64/" % VER + part) in out, "")

        # 发布
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/publish.sh"),
                       "--component", "all", "--version", VER, "--arch", "x86_64"], env)
        e2e.check("publish: 全部组件成功", rc == 0, out[-400:])

        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/publish.sh"),
                       "--component", "all", "--version", VER, "--arch", "x86_64"], env)
        e2e.check("publish: 重复执行幂等", rc == 0 and "skip" in out, out[-300:])

        # 自举：client 先装到自己
        p_client = os.path.join(LOG, "prefix-client")
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/rt-bootstrap.sh"),
                       "--component", "client", "--version", "latest", "--arch", "x86_64",
                       "--prefix", p_client, "--no-units"], env)
        e2e.check("bootstrap: client 安装成功", rc == 0, out[-400:])
        rc, out = run([os.path.join(p_client, "bin/rtunnel-client"), "--version"], env)
        e2e.check("bootstrap: client 版本正确", out.strip() == VER, out.strip())

        # 自举：relay
        p_relay = os.path.join(LOG, "prefix-relay")
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/rt-bootstrap.sh"),
                       "--component", "relay", "--version", "latest", "--arch", "x86_64",
                       "--prefix", p_relay, "--no-units"], env)
        e2e.check("bootstrap: relay 安装成功", rc == 0, out[-400:])
        rc, out = run([os.path.join(p_relay, "bin/rtunnel-relay"), "--version"], env)
        e2e.check("bootstrap: relay 版本正确", out.strip() == VER, out.strip())
        e2e.check("bootstrap: relay 带自举 client",
                  os.path.isfile(os.path.join(p_relay, "bin/rtunnel-client")), "")
        e2e.check("bootstrap: relay 带自举脚本",
                  os.path.isfile(os.path.join(p_relay, "rt-bootstrap.sh")), "")

        # 自举：agent（统一布局 bin/rtunnel-agent + manifest + SHA256SUMS）
        p_agent = os.path.join(LOG, "prefix-agent")
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/rt-bootstrap.sh"),
                       "--component", "agent", "--version", "latest", "--arch", "x86_64",
                       "--variant", "lite", "--prefix", p_agent, "--no-units"], env)
        e2e.check("bootstrap: agent 安装成功", rc == 0, out[-400:])
        rc, out = run([os.path.join(p_agent, "bin/rtunnel-agent"), "--version"], env)
        e2e.check("bootstrap: agent 版本正确", out.strip() == VER, out.strip())
        e2e.check("bootstrap: agent 带自举 client",
                  os.path.isfile(os.path.join(p_agent, "bin/rtunnel-client")), "")

        # status
        rc, out = run([os.path.join(p_relay, "install.sh"), "--status", "--prefix", p_relay], env)
        e2e.check("install: --status 显示当前版本", rc == 0 and "rtunnel-relay-%s" % VER in out, out)

        # bootstrap 的 --status 应委派给已安装的 install.sh
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/rt-bootstrap.sh"),
                       "--component", "relay", "--version", "latest", "--arch", "x86_64",
                       "--prefix", p_relay, "--no-units", "--status"], env)
        e2e.check("bootstrap: --status 委派给 install.sh",
                  rc == 0 and "rtunnel-relay-%s" % VER in out, out[-200:])

        # 篡改：把仓库里的 client 包写坏，bootstrap 必须拒绝
        bad = os.path.join(LOG, "files/packages/client/%s/x86_64" % VER,
                           ("rtunnel-client-%s-x86_64.tar.gz" % VER))
        with open(bad, "wb") as f:
            f.write(b"not a tarball" * 64)
        os.utime(bad, None)
        p_bad = os.path.join(LOG, "prefix-bad")
        rc, out = run(["bash", os.path.join(ROOT, "rtunnel/deploy/rt-bootstrap.sh"),
                       "--component", "client", "--version", "latest", "--arch", "x86_64",
                       "--prefix", p_bad, "--no-units"], env)
        e2e.check("bootstrap: 篡改包被拒", rc == 5, "rc=%d out=%s" % (rc, out[-200:]))

        # 回滚：伪造一个上一版实体 + previous，验证切换
        old = os.path.join(p_relay, "bin/rtunnel-relay-1.4.129")
        shutil.copy2(os.path.join(p_relay, "bin/rtunnel-relay"), old)
        with open(os.path.join(p_relay, "state/previous"), "w") as f:
            f.write("rtunnel-relay-1.4.129\trtunnel-relay.service\n")
        rc, out = run([os.path.join(p_relay, "install.sh"), "--rollback",
                       "--prefix", p_relay], env)
        cur = os.readlink(os.path.join(p_relay, "bin/rtunnel-relay"))
        e2e.check("install: 回滚切换软链", rc == 0 and cur == "rtunnel-relay-1.4.129",
                  "rc=%d cur=%s" % (rc, cur))
    finally:
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            relay.kill()
        log.close()


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
