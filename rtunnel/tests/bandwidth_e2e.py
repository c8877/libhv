#!/usr/bin/env python3
"""带宽整形 e2e：令牌设置了带宽上限后，隧道吞吐被限制且数据不丢。

场景：
  1. 不限速令牌传 128 KiB 往返 → 很快完成
  2. 800 kbps（100 KB/s）令牌传同样数据 → 明显变慢，但字节完整
  3. 管理接口回读 rate_kbps
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-bw"
PAYLOAD = 128 * 1024


def roundtrip(socks_ep, echo_port, payload):
    s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=5)
    if s is None:
        return None, "socks rep=%d" % rep
    try:
        t0 = time.time()
        s.sendall(payload)
        got = e2e.recv_exact(s, len(payload))
        return time.time() - t0, got
    finally:
        s.close()


def main():
    os.makedirs(LOG, exist_ok=True)
    for name in ("admin.db", "admin.db-wal", "admin.db-shm"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass
    filedir = os.path.join(LOG, "files")
    os.makedirs(filedir, exist_ok=True)

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    echo_port, socks_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port
    socks_ep = ("tcp", "127.0.0.1", socks_port)

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    logs = {k: open(os.path.join(LOG, k + ".log"), "w") for k in ("relay", "agent", "client")}
    echo = e2e.EchoServer(echo_port)
    echo.start()
    procs = []
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--drain-timeout", "300",
             "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "admintok",
             "--admin-db", os.path.join(LOG, "admin.db"),
             "--admin-file-dir", filedir, "--verbose"],
            stdout=logs["relay"], stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.6)

        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)

        _, call = e2e.admin_client(base)

        # 不限速
        tok_free = e2e.create_client_token(base, "bwfree")
        # 限速 800 kbps = 100 KB/s
        acc = call("POST", "/api/v1/client-accounts", {"username": "bwslow", "display_name": "bwslow"})
        call("POST", "/api/v1/permission/groups", {
            "name": "bw-slow-g", "description": "bw", "resources": e2e.E2E_RES,
            "all_devices": True, "devices": [], "ports": "*",
            "members": [{"type": "client", "id": str(acc["id"])}],
        })
        tok_slow = call("POST", "/api/v1/client-accounts/%d/tokens" % acc["id"],
                        {"name": "bw", "rate_kbps": 800})
        e2e.check("bw: 令牌带宽已保存", tok_slow.get("rate_kbps") == 800, str(tok_slow.get("rate_kbps")))

        def run(tok, tag):
            p = subprocess.Popen(
                [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
                 "--token", tok, "--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
                stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
            procs.append(p)
            # 等客户端就绪
            for _ in range(50):
                try:
                    s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=2)
                except OSError:
                    s = None
                if s is not None:
                    s.close()
                    break
                time.sleep(0.2)
            elapsed, got = roundtrip(socks_ep, echo_port, b"b" * PAYLOAD)
            p.send_signal(15)
            p.wait(timeout=5)
            procs.remove(p)
            e2e.check("bw[%s]: 数据完整" % tag, got is not None and len(got) == PAYLOAD,
                      "got=%r" % (len(got) if got else got))
            print("  [%s] 256 KiB 往返耗时 %.2fs" % (tag, elapsed or -1))
            return elapsed

        t_free = run(tok_free, "free")
        e2e.check("bw: 不限速令牌跑满", t_free is not None and t_free < 1.5,
                  "elapsed=%.2fs" % (t_free or -1))
        t_slow = run(tok_slow["token"], "800kbps")
        # 256 KiB / 100 KB/s ≈ 2.5s（上下行合计计费），留足余量
        e2e.check("bw: 限速令牌明显变慢", t_slow is not None and t_slow >= 1.5,
                  "elapsed=%.2fs" % (t_slow or -1))
        e2e.check("bw: 限速不是靠丢数据", t_slow is not None and t_slow < 30,
                  "elapsed=%.2fs" % (t_slow or -1))
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    p.send_signal(15)
                    p.wait(timeout=3)
                except (subprocess.TimeoutExpired, OSError):
                    p.kill()
        echo.close()
        for f in logs.values():
            f.flush()
            f.close()
    return e2e.finish()


if __name__ == "__main__":
    sys.exit(main())
