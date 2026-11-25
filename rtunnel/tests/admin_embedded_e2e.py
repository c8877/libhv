#!/usr/bin/env python3
"""rtunnel-relay-embedded：管理台前端固化在二进制里的行为。

普通 rtunnel-relay 只认 --admin-docroot；内嵌版在没有 docroot 时直接提供
构建期打包的 web/dist（.gz 预压缩 + 非 gzip 客户端懒解压）。
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e

EMBED = os.path.join(e2e.BIN, "rtunnel-relay-embedded")
PLAIN = os.path.join(e2e.BIN, "rtunnel-relay")


def fetch(url, encoding=None, timeout=5):
    req = urllib.request.Request(url)
    if encoding is not None:
        req.add_header("Accept-Encoding", encoding)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except OSError:
        return 0, {}, b""


def start_relay(binary, workdir, extra=None):
    port = e2e.free_port()
    admin_port = e2e.free_port()
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = e2e.LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    log = open(os.path.join(workdir, "relay.log"), "w")
    args = [binary, "--host", "127.0.0.1", "--port", str(port),
            "--admin", "127.0.0.1:%d" % admin_port,
            "--admin-token", "admintok",
            "--admin-db", os.path.join(workdir, "a.db"),
            "--admin-file-dir", os.path.join(workdir, "files")]
    if extra:
        args += extra
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env)
    base = "http://127.0.0.1:%d" % admin_port
    for _ in range(50):
        if proc.poll() is not None:
            break
        code, _, _ = fetch(base + "/", timeout=0.5)
        if code:
            return proc, base, log
        time.sleep(0.1)
    proc.terminate()
    raise RuntimeError("relay 未启动，日志见 %s/relay.log" % workdir)


def stop(proc, log):
    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    log.close()


def main():
    if not os.path.exists(EMBED):
        print("skip: %s 不存在（用 -DRTUNNEL_ADMIN_WEB=ON 构建 rtunnel-relay-embedded）" % EMBED)
        return 0
    if not os.path.exists(PLAIN):
        print("skip: %s 不存在" % PLAIN)
        return 0

    tmp = tempfile.mkdtemp(prefix="rtunnel-embed-")
    proc, base, log = start_relay(EMBED, tmp)

    # 1) 首页来自内嵌资源（没传 docroot）
    code, headers, body = fetch(base + "/")
    html = body.decode("utf-8", "replace")
    e2e.check("embed: 无 docroot 也能打开首页", code == 200 and "text/html" in headers.get("Content-Type", ""),
              "status=%s ctype=%s" % (code, headers.get("Content-Type")))
    e2e.check("embed: 首页是管理台", 'id="app"' in html or "<title>rtunnel</title>" in html, html[:120])

    # 2) 入口 JS：gzip 直出 / 无 gzip 解压
    m = re.search(r"/assets/[A-Za-z0-9._-]+\.js", html)
    e2e.check("embed: 首页引用 assets", m is not None, html[:200])
    asset = m.group(0) if m else "/assets/missing.js"
    code, headers, gz = fetch(base + asset, "gzip, deflate, br")
    e2e.check("embed: gzip 客户端拿到压缩体",
              code == 200 and headers.get("Content-Encoding") == "gzip" and gz[:2] == b"\x1f\x8b",
              "status=%s enc=%s len=%d" % (code, headers.get("Content-Encoding"), len(gz)))
    code, headers, plain = fetch(base + asset)
    e2e.check("embed: 无 gzip 客户端拿到解压正文",
              code == 200 and not headers.get("Content-Encoding") and b"const " in plain
              and plain[:2] != b"\x1f\x8b",
              "status=%s enc=%s len=%d" % (code, headers.get("Content-Encoding"), len(plain)))

    # 3) gzip;q=0 视为不接受
    code, headers, plain = fetch(base + asset, "gzip;q=0")
    e2e.check("embed: gzip;q=0 走解压", code == 200 and not headers.get("Content-Encoding"),
              "enc=%s" % headers.get("Content-Encoding"))

    # 4) 静态资源长缓存头部（与磁盘模式一致）
    code, headers, _ = fetch(base + asset, "gzip")
    e2e.check("embed: assets 可长缓存",
              headers.get("Cache-Control", "").find("immutable") >= 0, headers.get("Cache-Control"))

    # 5) 404 与目录穿越
    code, _, _ = fetch(base + "/assets/nothing.js")
    e2e.check("embed: 不存在的资源 404", code == 404, "status=%d" % code)
    code, _, _ = fetch(base + "/assets/../../etc/passwd")
    e2e.check("embed: 路径穿越被拒", code == 404, "status=%d" % code)
    stop(proc, log)

    # 6) docroot 优先，磁盘缺的资源回退内嵌（同一二进制）
    docroot = os.path.join(tmp, "web")
    os.makedirs(docroot, exist_ok=True)
    with open(os.path.join(docroot, "index.html"), "w") as f:
        f.write("<html><body>DISK-OVERRIDE</body></html>")
    proc, base, log = start_relay(EMBED, tmp, ["--admin-docroot", docroot])
    code, _, body = fetch(base + "/")
    e2e.check("embed: docroot 覆盖首页", code == 200 and b"DISK-OVERRIDE" in body,
              body[:80].decode("utf-8", "replace"))
    code, headers, body = fetch(base + asset, "gzip")
    e2e.check("embed: 磁盘缺失时回退内嵌",
              code == 200 and headers.get("Content-Encoding") == "gzip",
              "status=%s enc=%s" % (code, headers.get("Content-Encoding")))
    stop(proc, log)

    # 7) 普通 relay 行为不变：没有 docroot 时首页 404，并提示两种做法
    proc, base, log = start_relay(PLAIN, tmp)
    code, _, body = fetch(base + "/")
    text = body.decode("utf-8", "replace")
    e2e.check("plain: 无 docroot 首页 404", code == 404, "status=%d" % code)
    e2e.check("plain: 提示里给出两种部署方式",
              "admin-docroot" in text and "rtunnel-relay-embedded" in text, text.strip()[:160])
    stop(proc, log)

    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
