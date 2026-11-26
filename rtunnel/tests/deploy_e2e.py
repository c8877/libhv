#!/usr/bin/env python3
"""运维 → 部署 的端到端验收。

不需要 root，也不碰 ~/.ssh：
  1. 在 127.0.0.1:2222 起一个私有 sshd（AuthorizedKeysFile 在 /tmp 下）
  2. 起一个 relay，把本机当目标主机登记进去（私钥认证）
  3. 装 client 包到临时 prefix（--no-units），验证版本、日志、失败与取消路径

用法：python3 rtunnel/tests/deploy_e2e.py
跳过真实 SSH 部分：DEPLOY_TEST_SSH=0
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "rtunnel", "tests"))
import e2e  # noqa: E402  复用 free_port / admin_client / check

BIN = os.environ.get("RTUNNEL_BIN", os.path.join(ROOT, "build", "bin"))
LIB = os.environ.get("RTUNNEL_LIB", os.path.join(ROOT, "build", "lib"))
WORK = "/tmp/rtunnel-deploy-test"
SSH_DIR = os.path.join(WORK, "ssh")


def sh(cmd, **kw):
    return subprocess.check_output(cmd, shell=True, text=True, **kw)


def prepare_sshd(port):
    """私有 sshd：只认 /tmp 下的 authorized_keys，不读也不写 ~/.ssh。"""
    os.makedirs(SSH_DIR, exist_ok=True)
    host_key = os.path.join(SSH_DIR, "host_key")
    client_key = os.path.join(SSH_DIR, "client_key")
    auth = os.path.join(SSH_DIR, "authorized_keys")
    if not os.path.exists(host_key):
        sh("ssh-keygen -t ed25519 -N '' -f %s -q" % host_key)
    if not os.path.exists(client_key):
        # PEM RSA：走和线上一样的私钥认证路径
        sh("ssh-keygen -t rsa -b 2048 -m PEM -N '' -f %s -q" % client_key)
    # 另一把 ed25519：验证 openssh-key-v1 私钥格式也能用
    ed_key = os.path.join(SSH_DIR, "ed25519_key")
    if not os.path.exists(ed_key):
        sh("ssh-keygen -t ed25519 -N '' -f %s -q" % ed_key)
    with open(auth, "w") as f:
        f.write(open(client_key + ".pub").read())
        f.write(open(ed_key + ".pub").read())
    os.chmod(auth, 0o600)
    os.chmod(client_key, 0o600)
    cfg = os.path.join(SSH_DIR, "sshd_config")
    with open(cfg, "w") as f:
        f.write("""Port %d
ListenAddress 127.0.0.1
HostKey %s
PidFile %s
AuthorizedKeysFile %s
PermitRootLogin no
PasswordAuthentication no
PubkeyAuthentication yes
UsePAM no
StrictModes no
Subsystem sftp internal-sftp
LogLevel ERROR
""" % (port, host_key, os.path.join(SSH_DIR, "sshd.pid"), auth))
    p = subprocess.Popen(["/usr/sbin/sshd", "-f", cfg, "-E", os.path.join(SSH_DIR, "sshd.log")])
    for _ in range(30):
        time.sleep(0.1)
        try:
            s = socket.socket()
            s.connect(("127.0.0.1", port))
            s.close()
            return client_key, p, ed_key
        except OSError:
            continue
    raise RuntimeError("私有 sshd 没起来，看 " + os.path.join(SSH_DIR, "sshd.log"))


def make_package(repo, ver):
    """用真实 client 二进制打一个部署包，放进 relay 的仓库 deploy/ 目录。"""
    binp = os.path.join(ROOT, "build", "deploy", "agent", "bin", "rtunnel-client")
    if not os.path.exists(binp):
        return None
    # 版本必须与包内二进制一致，否则 install.sh 的版本自检会拒绝
    ver = sh("%s --version" % binp).strip() or ver
    stage = os.path.join(WORK, "pkg", "rtunnel-client-%s-x86_64" % ver)
    shutil.rmtree(os.path.dirname(stage), ignore_errors=True)
    os.makedirs(os.path.join(stage, "bin"))
    os.makedirs(os.path.join(stage, "etc"))
    os.makedirs(os.path.join(stage, "service"))
    shutil.copy2(binp, os.path.join(stage, "bin", "rtunnel-client"))
    shutil.copy2(os.path.join(ROOT, "rtunnel", "deploy", "client", "install.sh"),
                 os.path.join(stage, "install.sh"))
    shutil.copy2(os.path.join(ROOT, "rtunnel", "deploy", "client", "client.args.example"),
                 os.path.join(stage, "etc", "client.args.example"))
    shutil.copy2(os.path.join(ROOT, "rtunnel", "deploy", "client", "rtunnel-client-socks.service"),
                 os.path.join(stage, "service", "rtunnel-client-socks.service"))
    with open(os.path.join(stage, "manifest.json"), "w") as f:
        json.dump({"name": "rtunnel-client", "version": ver, "arch": "x86_64"}, f)
    sh("cd %s && find . -type f ! -name SHA256SUMS -printf '%%P\\n' | sort | xargs sha256sum > SHA256SUMS"
       % stage)
    outdir = os.path.join(repo, "deploy")
    os.makedirs(outdir, exist_ok=True)
    name = "rtunnel-client-%s-x86_64" % ver
    sh("tar --format=ustar -C %s -czf %s/%s.tar.gz %s" % (os.path.dirname(stage), outdir, name, name))
    return name + ".tar.gz"


def call(op, base, path, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method or ("POST" if data else "GET"))
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with op.open(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def wait_job(op, base, jid, timeout=180):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        code, j = call(op, base, "/api/v1/deploy/jobs/%d" % jid, method="GET")
        last = j
        if code == 200 and j.get("status") in ("success", "failed", "canceled"):
            return j
        time.sleep(0.3)
    return last


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    # 上次的 relay 库、前缀都清掉
    repo = os.path.join(WORK, "repo")
    os.makedirs(repo, exist_ok=True)
    logdir = os.path.join(WORK, "logs")
    os.makedirs(logdir, exist_ok=True)

    use_ssh = os.environ.get("DEPLOY_TEST_SSH", "1") != "0"
    sshd = None
    key_path = None
    ed_key = None
    ssh_port = 0
    if use_ssh:
        ssh_port = e2e.free_port()
        key_path, sshd, ed_key = prepare_sshd(ssh_port)

    pkg = make_package(repo, "0.0.0")
    pkg_ver = None
    if pkg:
        pkg_ver = pkg[len("rtunnel-client-"):-len("-x86_64.tar.gz")]
    e2e.check("deploy: 打包出 client 安装包", pkg is not None, "缺 build/deploy/agent/bin/rtunnel-client")

    info = {}
    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    rlog = open(os.path.join(logdir, "relay.log"), "w")
    relay = subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
         "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "admintok",
         "--admin-db", os.path.join(WORK, "admin.db"), "--admin-file-dir", repo],
        stdout=rlog, stderr=subprocess.STDOUT, env=env)
    try:
        time.sleep(0.8)
        op, _ = e2e.admin_client(base)

        # 1) 包列表：从文件名解析组件/版本/架构
        code, pk = call(op, base, "/api/v1/deploy/packages", method="GET")
        items = pk.get("items") or []
        e2e.check("deploy: 识别安装包",
                  code == 200 and any(i["name"] == pkg and i["component"] == "client" and
                                      i["version"] == pkg_ver and i["arch"] == "x86_64"
                                      for i in items), str(items)[:200])

        # 2) 参数校验
        code, r = call(op, base, "/api/v1/deploy/hosts",
                       {"host": "-oProxyCommand=x", "username": "u", "secret": "s"})
        e2e.check("deploy: 非法主机名被拒", code == 400, "code=%d r=%s" % (code, r))
        code, r = call(op, base, "/api/v1/deploy/hosts",
                       {"host": "10.0.0.1", "username": "u;rm -rf /", "secret": "s"})
        e2e.check("deploy: 非法用户名被拒", code == 400, "code=%d r=%s" % (code, r))

        # 扫描参数边界
        code, r = call(op, base, "/api/v1/deploy/scan", {"cidr": "8.8.8.0/24"})
        e2e.check("deploy: 扫描公网段被拒", code == 400 and "私有" in str(r),
                  "code=%d r=%s" % (code, r))
        code, r = call(op, base, "/api/v1/deploy/scan", {"cidr": "10.0.0.0/16"})
        e2e.check("deploy: 扫描过大网段被拒", code == 400 and "太大" in str(r),
                  "code=%d r=%s" % (code, r))
        code, r = call(op, base, "/api/v1/deploy/scan", {"cidr": "10.0.0.0/33"})
        e2e.check("deploy: 扫描非法掩码被拒", code == 400, "code=%d r=%s" % (code, r))
        code, r = call(op, base, "/api/v1/deploy/scan", {"cidr": "not-a-cidr"})
        e2e.check("deploy: 扫描非法地址被拒", code == 400, "code=%d r=%s" % (code, r))

        if use_ssh:
            with open(key_path) as f:
                keytext = f.read()
            code, h = call(op, base, "/api/v1/deploy/hosts",
                           {"name": "local-sshd", "host": "127.0.0.1", "port": ssh_port,
                            "username": os.environ.get("USER", "root"), "auth": "key",
                            "secret": keytext})
            info["host"] = h.get("id")
            e2e.check("deploy: 登记主机", code == 200 and h.get("id"), "code=%d r=%s" % (code, h))

            code, dup = call(op, base, "/api/v1/deploy/hosts",
                             {"name": "dup", "host": "127.0.0.1", "port": ssh_port,
                              "username": os.environ.get("USER", "root"), "auth": "key",
                              "secret": keytext})
            e2e.check("deploy: 同一地址重复登记被拒", code == 400, "code=%d r=%s" % (code, dup))

            code, lst = call(op, base, "/api/v1/deploy/hosts", method="GET")
            row = (lst.get("items") or [{}])[0]
            e2e.check("deploy: 列表不回显凭据",
                      code == 200 and "secret" not in row and row.get("secret_set") is True,
                      str(row.keys()))

            # 2.5) 网段扫描：私有 sshd 就在 127.0.0.1:<ssh_port>
            code, sc = call(op, base, "/api/v1/deploy/scan",
                            {"cidr": "127.0.0.1/32", "port": ssh_port})
            hit = [i for i in (sc.get("items") or [])
                   if i.get("host") == "127.0.0.1" and i.get("port") == ssh_port]
            # 非 22 端口时 nmap 只能报 tcp（服务名来自端口库），SSH 由横幅确认见下面的 TCP 扫描
            e2e.check("deploy: 扫描到本机 sshd",
                      code == 200 and len(hit) == 1 and hit[0].get("service") in ("ssh", "tcp"),
                      "code=%d r=%s" % (code, str(sc)[:300]))
            e2e.check("deploy: 扫到的主机标为已登记",
                      bool(hit) and hit[0].get("known") is True and
                      hit[0].get("host_id") == info["host"], str(hit)[:200])

            # 强制内置 TCP 扫描（没装 nmap 的镜像走的就是这条）
            code, sc2 = call(op, base, "/api/v1/deploy/scan",
                             {"cidr": "127.0.0.1/32", "port": ssh_port, "engine": "tcp"})
            e2e.check("deploy: 内置 TCP 扫描读到 SSH 横幅",
                      code == 200 and sc2.get("engine") == "tcp" and
                      any(i["host"] == "127.0.0.1" and i.get("service") == "ssh"
                          for i in sc2.get("items") or []),
                      "code=%d r=%s" % (code, str(sc2)[:300]))

            if shutil.which("nmap"):
                code, sc3 = call(op, base, "/api/v1/deploy/scan",
                                 {"cidr": "127.0.0.1/32", "port": ssh_port, "engine": "nmap"})
                e2e.check("deploy: nmap 扫描可用",
                          code == 200 and sc3.get("engine") == "nmap" and
                          any(i["host"] == "127.0.0.1" for i in sc3.get("items") or []),
                          "code=%d r=%s" % (code, str(sc3)[:300]))

            # 关闭的端口：能扫完，但没有结果
            code, sc4 = call(op, base, "/api/v1/deploy/scan",
                             {"cidr": "127.0.0.1/32", "port": e2e.free_port()})
            e2e.check("deploy: 没有 SSH 时返回空列表",
                      code == 200 and sc4.get("items") == [], "code=%d r=%s" % (code, str(sc4)[:200]))

            # 3) 探测
            code, pr = call(op, base, "/api/v1/deploy/hosts/%d/probe" % info["host"], {}, )
            e2e.check("deploy: 连接测试通过",
                      code == 200 and pr.get("ok") is True and pr.get("arch") == "x86_64",
                      str(pr)[:200])

            # 4) 部署 client 到临时前缀（只装文件）
            prefix = os.path.join(WORK, "inst")
            code, job = call(op, base, "/api/v1/deploy/jobs",
                             {"host_id": info["host"], "component": "client", "package": pkg,
                              "prefix": prefix, "no_units": True,
                              "options": {"server": "127.0.0.1:18443"}})
            e2e.check("deploy: 提交作业", code == 200 and job.get("id"), "code=%d r=%s" % (code, job))
            jid = job.get("id")
            info["job"] = jid
            done = wait_job(op, base, jid)
            e2e.check("deploy: 作业成功", done.get("status") == "success",
                      "status=%s stage=%s err=%s log=%s" %
                      (done.get("status"), done.get("step"), done.get("error"),
                       (done.get("log") or "")[-600:]))
            log = done.get("log") or ""
            for step in ("预检", "上传", "解包", "配置", "安装", "验证"):
                e2e.check("deploy: 经过「%s」步骤" % step, step in log, log[-300:])
            e2e.check("deploy: 临时目录已清理",
                      not os.path.exists("/tmp/rtunnel-deploy-%d" % jid))
            installed = os.path.join(prefix, "bin", "rtunnel-client")
            e2e.check("deploy: 二进制已安装", os.path.exists(installed), installed)
            e2e.check("deploy: 配置已写入",
                      os.path.exists(os.path.join(prefix, "etc", "client.env")))
            if os.path.exists(installed):
                ver = sh("%s --version" % installed).strip()
                e2e.check("deploy: 版本可执行", ver == pkg_ver, "%s vs %s" % (ver, pkg_ver))

            # 4.5) ed25519（openssh-key-v1 私钥）也要能认证：同一主机换凭据
            with open(ed_key) as f:
                edtext = f.read()
            code, _ = call(op, base, "/api/v1/deploy/hosts/%d" % info["host"],
                           {"auth": "key", "secret": edtext}, method="PUT")
            code, pr3 = call(op, base, "/api/v1/deploy/hosts/%d/probe" % info["host"], {})
            e2e.check("deploy: ed25519 私钥可用",
                      code == 200 and pr3.get("ok") is True, str(pr3)[:200])

            # 4.6) 密码认证走同一条通道：密码错要给出明确原因（不是“连不上”）
            code, _ = call(op, base, "/api/v1/deploy/hosts/%d" % info["host"],
                           {"auth": "password", "secret": "wrong-password"}, method="PUT")
            code, pr4 = call(op, base, "/api/v1/deploy/hosts/%d/probe" % info["host"], {})
            e2e.check("deploy: 密码错时说明认证失败",
                      code != 200 and "认证失败" in json.dumps(pr4, ensure_ascii=False),
                      "code=%d r=%s" % (code, pr4))
            # 还原成能用的私钥，后面的主机密钥检查还要连
            code, _ = call(op, base, "/api/v1/deploy/hosts/%d" % info["host"],
                           {"auth": "key", "secret": keytext}, method="PUT")

            # 5) 主机密钥变化必须拒绝
            code, _ = call(op, base, "/api/v1/deploy/hosts/%d/probe" % info["host"], {})
            kh = os.path.join(WORK, "deploy", "known_hosts")
            e2e.check("deploy: 记录了主机密钥", os.path.exists(kh))
            if os.path.exists(kh):
                with open(kh) as f:
                    lines = f.readlines()
                with open(kh, "w") as f:
                    f.writelines(l.replace("sha256:", "sha256:AAAA") for l in lines)
                code, bad = call(op, base, "/api/v1/deploy/hosts/%d/probe" % info["host"], {})
                e2e.check("deploy: 主机密钥变了直接拒绝",
                          code != 200 and "主机密钥" in json.dumps(bad, ensure_ascii=False),
                          "code=%d r=%s" % (code, bad))
                with open(kh, "w") as f:
                    f.writelines(lines)

        # 6) 连不上的主机：作业要失败并说清原因
        code, h2 = call(op, base, "/api/v1/deploy/hosts",
                        {"name": "dead", "host": "127.0.0.1", "port": 1,
                         "username": "nobody", "auth": "password", "secret": "x"})
        e2e.check("deploy: 登记不可达主机", code == 200 and h2.get("id"), str(h2))
        if h2.get("id"):
            code, j2 = call(op, base, "/api/v1/deploy/jobs",
                            {"host_id": h2["id"], "component": "agent", "package": pkg,
                             "prefix": "/tmp/nope",
                             "options": {"devid": "d1", "server": "1.2.3.4:18443"}})
            e2e.check("deploy: 包与组件不匹配被拒", code in (400, 404), "code=%d r=%s" % (code, j2))
            code, j2 = call(op, base, "/api/v1/deploy/jobs",
                            {"host_id": h2["id"], "component": "client", "package": pkg,
                             "prefix": "/tmp/nope", "no_units": True, "options": {}})
            if code == 200:
                d2 = wait_job(op, base, j2["id"], 60)
                e2e.check("deploy: 连不上时作业失败并说明",
                          d2.get("status") == "failed" and "连不上目标主机" in (d2.get("error") or ""),
                          "status=%s err=%s" % (d2.get("status"), d2.get("error")))

        # 7) 包不存在
        code, r = call(op, base, "/api/v1/deploy/jobs",
                       {"host_id": 1, "component": "client", "package": "rtunnel-client-9.9.9-x86_64.tar.gz",
                        "options": {}})
        e2e.check("deploy: 不存在的包被拒", code in (400, 404), "code=%d r=%s" % (code, r))

        # 8) 权限：只读账号看不到写接口
        code, _ = call(op, base, "/api/v1/deploy/hosts", method="GET")
        e2e.check("deploy: 管理员可读主机", code == 200)
    finally:
        # 清理：删主机（凭据文件一起删）
        try:
            for k in ("host", "host3", "host4"):
                if info.get(k):
                    call(op, base, "/api/v1/deploy/hosts/%d" % info[k], method="DELETE")
            cred = os.path.join(WORK, "deploy", "%d.key" % info.get("host", 0))
            e2e.check("deploy: 删除主机时清掉凭据", not os.path.exists(cred), cred)
        except Exception:
            pass
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except Exception:
            relay.kill()
        rlog.close()
        if sshd:
            sshd.terminate()
        if os.environ.get("DEPLOY_KEEP") != "1":
            shutil.rmtree(WORK, ignore_errors=True)
    e2e.finish()


if __name__ == "__main__":
    main()
