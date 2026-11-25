#!/usr/bin/env python3
"""权限回归：混合权限组的端口范围、WS 消息路径的身份来源。

1) 只授予 file.read 的组（ports 默认 "*"）不能放大另一个组限定的端口范围；
2) 文件部署 WS 的写权限必须按会话在打开时判定，不能被同 worker 线程上
   其它请求的 tl_actor 覆盖（--admin-threads 1 时是确定性场景）。

依赖：build/bin/rtunnel-relay、build/bin/rtunnel-agent，纯标准库。
"""

import base64
import http.cookiejar
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.environ.get("RTUNNEL_BIN", os.path.join(ROOT, "build", "bin"))

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS  " if cond else "FAIL  ") + name + (("  [" + str(detail) + "]") if detail and not cond else ""))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http_session(base):
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    def call(method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(base + path, data=data, method=method)
        if data:
            req.add_header("Content-Type", "application/json")
        try:
            with op.open(req, timeout=5) as r:
                return json.loads(r.read().decode() or "null")
        except urllib.error.HTTPError as e:
            return {"_status": e.code, "_body": e.read().decode()}

    return jar, call


# ---------------------------------------------------------------- 最小 WS 客户端

def ws_open(host, port, path, cookie):
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nCookie: rt_sid=%s\r\n\r\n"
           % (path, host, port, key, cookie))
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    status = resp.split(b"\r\n", 1)[0] if resp else b""
    return s, status


def ws_send(s, text):
    data = text.encode()
    mask = os.urandom(4)
    n = len(data)
    if n < 126:
        head = bytes([0x81, 0x80 | n])
    elif n < 65536:
        head = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def ws_recv(s, timeout=5):
    s.settimeout(timeout)
    while True:
        head = s.recv(2)
        if len(head) < 2:
            return None
        op = head[0] & 0x0F
        n = head[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", s.recv(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", s.recv(8))[0]
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        if op == 0x1:
            return buf.decode()
        if op == 0x8:
            return None
        # ping/pong/其它帧：跳过


def push_open(s, label):
    ws_send(s, json.dumps({"op": "push_open", "id": 1, "repo": "x", "path": "/tmp/x"}))
    raw = ws_recv(s)
    if not raw:
        return None
    try:
        return json.loads(raw).get("message", "")
    except ValueError:
        return raw


def items(v):
    if isinstance(v, list):
        return v
    for k in ("items", "list", "agents", "users"):
        if isinstance(v, dict) and isinstance(v.get(k), list):
            return v[k]
    return []


def main():
    relay_p, admin_p, echo_p = free_port(), free_port(), free_port()
    tmp = tempfile.mkdtemp(prefix="rt-perm-")
    os.makedirs(os.path.join(tmp, "files"), exist_ok=True)
    base = "http://127.0.0.1:%d" % admin_p
    logs = [open(os.path.join(tmp, "relay.log"), "wb"), open(os.path.join(tmp, "agent.log"), "wb")]
    procs = []
    try:
        procs.append(subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_p),
             "--admin", "127.0.0.1:%d" % admin_p, "--admin-token", "t0k",
             "--admin-db", os.path.join(tmp, "admin.db"),
             "--admin-file-dir", os.path.join(tmp, "files"),
             "--admin-threads", "1"],
            stdout=logs[0], stderr=subprocess.STDOUT))
        procs.append(subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_p),
             "--devid", "dev1", "--allow", str(echo_p),
             "--config-file", os.path.join(tmp, "agent.json")],
            stdout=logs[1], stderr=subprocess.STDOUT))

        for _ in range(100):
            try:
                _, call = http_session(base)
                if call("GET", "/api/v1/setup/status").get("initialized") is not None:
                    break
            except Exception:
                pass
            time.sleep(0.1)

        _, call = http_session(base)
        call("POST", "/api/v1/setup", {"token": "t0k", "username": "root", "password": "rootpass1"})
        _, call = http_session(base)
        login = call("POST", "/api/v1/login", {"username": "root", "password": "rootpass1"})
        check("admin login", login and login.get("_status") is None, login)

        for _ in range(100):
            if any(d.get("devid") == "dev1" for d in items(call("GET", "/api/v1/agents"))):
                break
            time.sleep(0.1)
        online = any(d.get("devid") == "dev1" for d in items(call("GET", "/api/v1/agents")))
        check("agent online", online)

        def group(name, resources, ports, global_scope=False):
            r = call("POST", "/api/v1/permission/groups", {
                "name": name, "description": "", "resources": resources,
                "all_devices": False, "devices": ["dev1"], "global_scope": global_scope,
                "ports": ports, "members": []})
            return r.get("id")

        g_p22 = group("p22", ["device.view", "device.port"], "22")
        g_files = group("files", ["file.read"], "*")
        g_ro = group("ro", ["file.read"], "*")
        g_rw = group("rw", ["file.read", "file.write", "repo.read"], "*", global_scope=True)
        check("groups created", all([g_p22, g_files, g_ro, g_rw]), [g_p22, g_files, g_ro, g_rw])

        # ---- 端口范围：混合组不能放大
        acc = call("POST", "/api/v1/client-accounts", {"username": "c1", "display_name": "", "note": ""})
        aid = acc.get("id")
        check("client account created", bool(aid), acc)

        def eff_ports(ids):
            call("PUT", "/api/v1/permission/subjects/client/%d/groups" % aid, {"ids": ids})
            eff = call("GET", "/api/v1/client-accounts/%d/effective" % aid)
            return eff.get("caps", {}).get("tcp_ports")

        check("ports: 只给文件权限的组 -> 无端口", eff_ports([g_files]) == "")
        check("ports: 文件组 + 22 端口组 -> 22", eff_ports([g_files, g_p22]) == "22",
              eff_ports([g_files, g_p22]))
        check("ports: 只给端口组 -> 22", eff_ports([g_p22]) == "22")

        # ---- WS 部署权限按会话判定
        call("POST", "/api/v1/users", {"username": "low", "password": "pw123456", "role": "viewer"})
        call("POST", "/api/v1/users", {"username": "ops", "password": "pw123456", "role": "viewer"})
        users = items(call("GET", "/api/v1/users"))
        uid = {u.get("username"): u.get("id") for u in users}
        call("PUT", "/api/v1/users/%d/perm-groups" % uid["low"], {"ids": [g_ro]})
        call("PUT", "/api/v1/users/%d/perm-groups" % uid["ops"], {"ids": [g_rw]})

        low_jar, low_call = http_session(base)
        low_call("POST", "/api/v1/login", {"username": "low", "password": "pw123456"})
        low_sid = next((c.value for c in low_jar if c.name == "rt_sid"), None)
        ops_jar, ops_call = http_session(base)
        ops_call("POST", "/api/v1/login", {"username": "ops", "password": "pw123456"})
        ops_sid = next((c.value for c in ops_jar if c.name == "rt_sid"), None)
        check("login low/ops", bool(low_sid and ops_sid))

        low_ws, low_st = ws_open("127.0.0.1", admin_p, "/ws/fs/dev1", low_sid)
        check("low opens fs ws", b"101" in low_st, low_st)
        msg = push_open(low_ws, "low none")
        check("low 无部署权限", msg == "没有部署权限", msg)
        call("GET", "/api/v1/overview")   # 同线程上换掉 tl_actor
        msg = push_open(low_ws, "low after admin")
        check("low 权限不被其它请求放大", msg == "没有部署权限", msg)

        ops_ws, ops_st = ws_open("127.0.0.1", admin_p, "/ws/fs/dev1", ops_sid)
        check("ops opens fs ws", b"101" in ops_st, ops_st)
        low_call("GET", "/api/v1/me")     # 同线程上换掉 tl_actor
        msg = push_open(ops_ws, "ops after low")
        check("ops 部署权限不被其它请求吞掉", msg is not None and msg != "没有部署权限", msg)

        low_ws.close()
        ops_ws.close()
    finally:
        for p in reversed(procs):
            p.terminate()
        for p in reversed(procs):
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        for f in logs:
            f.close()
        shutil.rmtree(tmp, ignore_errors=True)

    failed = [r for r in results if not r[1]]
    print("\n%d/%d passed" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
