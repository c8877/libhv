#!/usr/bin/env python3
"""用量计量 e2e：流量 / 会话 / 设备在线 三类用量落流水与日聚合，并验证权限。

场景：
  1. 客户端经隧道传 4 KiB，echo 回 4 KiB → token bytes>=8192
  2. 会话关闭 → sessions>=1、session_seconds>=1
  3. agent 下线 → device online_seconds>=1（断开时冲掉尾巴）
  4. usage_events / usage_daily 都有行；client_tokens.used_bytes 已落库
  5. 没有 usage.read 的 rtk 令牌访问报表 → 403
"""

import os
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-usage"


def wait_roundtrip(socks_ep, echo_port, timeout):
    end = time.time() + timeout
    last = ""
    while time.time() < end:
        try:
            s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=3)
        except OSError as e:
            last = str(e)
            time.sleep(0.2)
            continue
        if s is None:
            last = "socks rep=%d" % rep
            time.sleep(0.2)
            continue
        try:
            s.sendall(b"ping")
            got = e2e.recv_exact(s, 4)
            if got == b"ping":
                return True, ""
            last = "echo=%r" % got
        except OSError as e:
            last = str(e)
        finally:
            s.close()
        time.sleep(0.2)
    return False, last


def usage_rows(call, query):
    return call("GET", "/api/v1/usage?" + query).get("rows", [])


def qty_of(rows, metric):
    return sum(int(r.get("qty", 0)) for r in rows if r.get("metric") == metric)


def bearer_status(url, token):
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


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
    logs = {k: open(os.path.join(LOG, k + ".log"), "w")
            for k in ("relay", "agent", "client")}
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

        tok = e2e.create_client_token(base, "meter")
        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)
        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--token", tok, "--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
        procs.append(client)

        ok, detail = wait_roundtrip(socks_ep, echo_port, 15)
        e2e.check("usage: 基线连通", ok, detail)

        # 一条能计时长的会话：传 4 KiB，等 1.2s 再关
        s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=5)
        e2e.check("usage: 建立会话", s is not None, "rep=%d" % rep)
        if s is not None:
            payload = b"u" * 4096
            s.sendall(payload)
            got = e2e.recv_exact(s, len(payload))
            e2e.check("usage: 数据往返", got == payload, "got=%d" % len(got))
            time.sleep(1.2)
            s.close()

        # 管理面 worker 5s 一轮：等它取走事件并落库
        time.sleep(6.5)
        _, call = e2e.admin_client(base)

        rows = usage_rows(call, "metric=bytes&subject_type=token")
        sent = qty_of(rows, "bytes")
        e2e.check("usage: token 流量已计量 >=8192", sent >= 8192, "bytes=%d" % sent)

        rows = usage_rows(call, "metric=sessions&subject_type=token")
        e2e.check("usage: 会话次数 >=1", qty_of(rows, "sessions") >= 1,
                  "sessions=%d" % qty_of(rows, "sessions"))

        rows = usage_rows(call, "metric=session_seconds&subject_type=token")
        e2e.check("usage: 会话时长 >=1s", qty_of(rows, "session_seconds") >= 1,
                  "seconds=%d" % qty_of(rows, "session_seconds"))

        # 汇总口径与按日口径要一致
        total = qty_of(usage_rows(call, "metric=bytes&granularity=total"), "bytes")
        e2e.check("usage: total 与按日汇总一致", total == sent, "total=%d day=%d" % (total, sent))

        # agent 下线：剩余在线时长立即冲掉，下一轮落库
        agent.send_signal(15)
        agent.wait(timeout=5)
        time.sleep(6.5)
        rows = usage_rows(call, "metric=online_seconds&subject_type=device&subject_id=dev1")
        online = qty_of(rows, "online_seconds")
        e2e.check("usage: 设备在线时长已计量", online >= 1, "seconds=%d" % online)

        # 流水与日聚合都落盘；令牌 used_bytes 也同步了
        db = sqlite3.connect(os.path.join(LOG, "admin.db"))
        ev = db.execute("SELECT COUNT(*) FROM usage_events").fetchone()[0]
        dl = db.execute("SELECT COUNT(*) FROM usage_daily").fetchone()[0]
        used = db.execute("SELECT used_bytes FROM client_tokens").fetchone()[0]
        db.close()
        e2e.check("usage: usage_events 有流水", ev > 0, "events=%d" % ev)
        e2e.check("usage: usage_daily 有聚合", dl > 0, "rows=%d" % dl)
        e2e.check("usage: client_tokens.used_bytes 已落库", used >= 8192, "used=%d" % used)

        # 权限：没有 usage.read 的 rtk 令牌不能看报表；管理员 atk 可以
        low = e2e.create_client_token(base, "lowuser", grant_all=False)
        code = bearer_status(base + "/api/v1/usage?metric=bytes", low)
        e2e.check("usage: 无权限令牌被拒", code == 403, "status=%d" % code)
        atk = call("POST", "/api/v1/admin-tokens", {"name": "usage-test"})["token"]
        code = bearer_status(base + "/api/v1/usage?metric=bytes", atk)
        e2e.check("usage: 授权令牌可查", code == 200, "status=%d" % code)
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
