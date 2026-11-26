#!/usr/bin/env python3
"""事件流（SSE）与分钟级指标。

覆盖：
  - /api/v1/events 首帧 hello、设备上下线、会话建立/结束、审计事件
  - Last-Event-ID 续传（只返回 cursor 之后的事件）
  - 受限账号只看自己设备的事件
  - /api/v1/agents/:devid/metrics/history 采样点、分桶与参数校验
  - agent 重启后 device.offline + device.online
"""

import http.client
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-events"


def sse_open(port, atk, last=None, timeout=5):
    """裸 socket 打开 SSE：http.client 在超时后会作废 fp，长连接读不了一样。"""
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    path = "/api/v1/events" + ("?last=%s" % last if last else "")
    req = ("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\n"
           "Accept: text/event-stream\r\nConnection: close\r\n\r\n" % (path, atk))
    sock.sendall(req.encode())
    buf = b""
    sock.settimeout(3)
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ")[1])
    except Exception:
        status = 0
    return sock, status, rest


def sse_collect(port, atk, seconds, last=None, stop_on=()):
    """读一段 SSE，返回 (status, events)。events 是 [{event,id,data}]。"""
    sock, status, rest = sse_open(port, atk, last)
    if status != 200:
        sock.close()
        return status, [{"data": rest.decode("utf-8", "replace")}]
    events, cur = [], {}
    buf = rest
    end = time.time() + seconds
    sock.settimeout(0.3)
    done = False
    while not done and time.time() < end:
        # 先消费缓冲里已有的完整行
        while b"\n" in buf:
            raw, _, buf = buf.partition(b"\n")
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if line == "":
                if cur:
                    events.append(cur)
                    if stop_on and cur.get("event") in stop_on:
                        done = True
                    cur = {}
                continue
            if line.startswith(":"):
                continue
            k, _, v = line.partition(": ")
            cur[k] = v
            if done:
                break
        if done:
            break
        try:
            chunk = sock.recv(4096)
        except (socket.timeout, TimeoutError):
            continue
        if not chunk:
            break
        buf += chunk
    sock.close()
    return 200, events


def ev_names(events):
    return [e.get("event", "") for e in events if e.get("event")]


def ev_data(e):
    try:
        return json.loads(e.get("data", "{}"))
    except Exception:
        return {}


def sse_status(port, atk):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", "/api/v1/events", headers={"Authorization": "Bearer " + atk})
    r = conn.getresponse()
    r.read()
    conn.close()
    return r.status


def bearer_get(base, path, token):
    req = urllib.request.Request(base + path, headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def wait_agent(base, devid, timeout=15):
    _, call = e2e.admin_client(base)
    end = time.time() + timeout
    while time.time() < end:
        try:
            items = call("GET", "/api/v1/agents")
            if isinstance(items, dict):
                items = items.get("items", [])
            for d in items:
                if d.get("devid") == devid and d.get("online"):
                    return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def main():
    os.makedirs(LOG, exist_ok=True)
    for name in ("admin.db", "admin.db-wal", "admin.db-shm", "agent.json"):
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

        adm_op, call = e2e.admin_client(base)
        atk = call("POST", "/api/v1/admin-tokens", {"name": "events-test"})["token"]
        e2e.check("events: 未授权被拒", sse_status(admin_port, "atk-bad") == 401, "")

        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)
        e2e.check("events: dev1 上线", wait_agent(base, "dev1"), "dev1 offline")

        # 1) 首帧 hello + 设备上线
        code, evs = sse_collect(admin_port, atk, 2.5)
        names = ev_names(evs)
        e2e.check("events: 首帧是 hello", names[:1] == ["hello"], str(names[:3]))
        e2e.check("events: 有设备上线事件", "device.online" in names, str(names[:6]))
        online = [ev_data(e) for e in evs if e.get("event") == "device.online"]
        e2e.check("events: 上线事件带 devid",
                  any(x.get("devid") == "dev1" for x in online), str(online)[:200])

        # 2) 会话建立 / 结束（走 client 的 SOCKS）
        tok = e2e.create_client_token(base, "evtuser")
        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--token", tok, "--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
        procs.append(client)
        for _ in range(50):
            try:
                s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=2)
            except OSError:
                s = None
            if s is not None:
                break
            time.sleep(0.2)
        code, evs = sse_collect(admin_port, atk, 2.0, stop_on=("stream.open",))
        e2e.check("events: 有会话建立事件", "stream.open" in ev_names(evs), str(ev_names(evs)))
        opened = [ev_data(e) for e in evs if e.get("event") == "stream.open"]
        e2e.check("events: 会话事件带端口",
                  any(x.get("port") == echo_port for x in opened), str(opened)[:200])
        if s is not None:
            s.close()
        code, evs = sse_collect(admin_port, atk, 3.0, stop_on=("stream.close",))
        e2e.check("events: 有会话结束事件", "stream.close" in ev_names(evs), str(ev_names(evs)))
        closed = [ev_data(e) for e in evs if e.get("event") == "stream.close"]
        e2e.check("events: 会话结束带字节与时长",
                  any("bytes_up" in x and "duration_ms" in x for x in closed), str(closed)[:200])

        # 3) 审计事件：先拿 cursor，再触发动作，再从 cursor 回放（总线是环形缓冲）
        code, evs0 = sse_collect(admin_port, atk, 1.0)
        cur0 = 0
        for e in evs0:
            if e.get("event") == "hello":
                cur0 = ev_data(e).get("cursor", 0)
        call("PUT", "/api/v1/settings", {"alert.cooldown_s": "301"})
        time.sleep(0.3)
        code, evs = sse_collect(admin_port, atk, 2.0, last=cur0, stop_on=("audit",))
        auds = [ev_data(e) for e in evs if e.get("event") == "audit"]
        e2e.check("events: 审计事件带 action/actor",
                  any(a.get("action") == "settings.update" and a.get("actor") for a in auds),
                  str(auds)[:240])

        # 4) 续传：从 hello 的 cursor 之后只拿新事件
        code, evs = sse_collect(admin_port, atk, 1.2)
        hello = [ev_data(e) for e in evs if e.get("event") == "hello"]
        cursor = hello[0].get("cursor", 0) if hello else 0
        e2e.check("events: hello 带 cursor", cursor > 0, str(hello)[:160])
        call("PUT", "/api/v1/settings", {"alert.cooldown_s": "302"})
        time.sleep(0.5)
        code, evs = sse_collect(admin_port, atk, 1.5, last=cursor, stop_on=("audit",))
        ids = [int(e["id"]) for e in evs if e.get("id")]
        e2e.check("events: 续传只返回 cursor 之后的事件",
                  bool(ids) and all(i > cursor for i in ids), "ids=%s cursor=%s" % (ids, cursor))

        # 5) 分钟级指标：上线即写一条
        now_ms = int(time.time() * 1000)
        code, hist = bearer_get(base, "/api/v1/agents/dev1/metrics/history?from=%d&to=%d&step=60"
                                % (now_ms - 900000, now_ms + 60000), atk)
        e2e.check("metrics: 历史查询成功", code == 200 and hist.get("devid") == "dev1",
                  "code=%d body=%s" % (code, str(hist)[:160]))
        pts = hist.get("points", [])
        e2e.check("metrics: 有采样点", len(pts) >= 1, "points=%d" % len(pts))
        if pts:
            p = pts[-1]
            e2e.check("metrics: 字段完整",
                      all(k in p for k in ("ts", "cpu", "mem", "mem_total", "load1", "rx", "tx")),
                      str(p)[:200])
            e2e.check("metrics: cpu 在 0-100", 0 <= p.get("cpu", -1) <= 100, str(p.get("cpu")))
        code, hist5 = bearer_get(base, "/api/v1/agents/dev1/metrics/history?from=%d&to=%d&step=300"
                                 % (now_ms - 900000, now_ms + 60000), atk)
        bpts = hist5.get("points", [])
        e2e.check("metrics: 分桶生效",
                  all(int(x["ts"]) % 300000 == 0 for x in bpts), str([x["ts"] for x in bpts])[:160])

        # 6) 参数校验：起点晚于终点、区间超过 31 天都拒
        code, r = bearer_get(base, "/api/v1/agents/dev1/metrics/history?from=%d&to=%d&step=60"
                             % (now_ms + 3600000, now_ms), atk)
        e2e.check("metrics: 起点晚于终点被拒", code == 400, "code=%d r=%s" % (code, str(r)[:100]))
        code, r = bearer_get(base, "/api/v1/agents/dev1/metrics/history?from=%d&to=%d&step=60"
                             % (now_ms - 40 * 86400000, now_ms), atk)
        e2e.check("metrics: 区间过大被拒", code == 400, "code=%d r=%s" % (code, str(r)[:100]))

        # 7) 受限账号：只能看到自己设备的事件
        t = call("POST", "/api/v1/tenants", {"name": "evt-tenant"})
        acc = call("POST", "/api/v1/client-accounts", {"username": "evtlow", "tenant_id": t["id"]})
        call("PUT", "/api/v1/permission/devices/nobody",
             {"name": "nobody", "enabled": True, "tenant_id": t["id"]})
        call("POST", "/api/v1/permission/groups", {
            "name": "evt-group", "description": "events",
            "resources": ["device.view", "device.exec"],
            "all_devices": False, "devices": ["nobody"], "ports": "*",
            "members": [{"type": "client", "id": str(acc["id"])}],
        })
        rtk = call("POST", "/api/v1/client-accounts/%d/tokens" % acc["id"], {"name": "e"})["token"]
        call("PUT", "/api/v1/settings", {"alert.cooldown_s": "303"})
        time.sleep(0.3)
        code, evs = sse_collect(admin_port, rtk, 1.5)
        names = ev_names(evs)
        e2e.check("events: 接入令牌能连事件流", code == 200 and "hello" in names, str(names[:3]))
        leaked = [e for e in evs if e.get("event") in ("device.online", "device.offline",
                                                      "stream.open", "stream.close", "audit")]
        e2e.check("events: 受限账号收不到别的设备事件", not leaked, str(leaked)[:200])

        # 8) 设备重启 → 离线 + 上线
        agent.send_signal(15)
        try:
            agent.wait(timeout=5)
        except Exception:
            agent.kill()
        code, evs = sse_collect(admin_port, atk, 5.0, stop_on=("device.offline",))
        e2e.check("events: 有设备离线事件", "device.offline" in ev_names(evs), str(ev_names(evs)))

        agent2 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent2)
        e2e.check("events: 设备重新上线", wait_agent(base, "dev1"), "dev1 offline")
        code, evs = sse_collect(admin_port, atk, 3.0, stop_on=("device.online",))
        e2e.check("events: 重连再次产生上线事件", "device.online" in ev_names(evs),
                  str(ev_names(evs)))
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    p.send_signal(15)
                    p.wait(timeout=3)
                except Exception:
                    p.kill()
        for f in logs.values():
            f.close()


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
