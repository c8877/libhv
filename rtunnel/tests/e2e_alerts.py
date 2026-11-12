#!/usr/bin/env python3
"""告警 e2e：本地 webhook sink + relay 管理台。

覆盖：
  - 通用 webhook 的测试消息、HMAC 签名
  - 企业微信/钉钉/飞书三种预置格式
  - 设备离线告警（offline_after_s=30）
  - 证书到期告警（伪造 index.txt）
  - viewer 读设置时的脱敏
"""

import hashlib
import hmac as hm
import http.server
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-e2e-alerts-%d" % os.getpid()
ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = LIB + ":" + ENV.get("LD_LIBRARY_PATH", "")


class Sink(http.server.BaseHTTPRequestHandler):
    events = []

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        Sink.events.append({
            "path": self.path,
            "body": body,
            "sig": self.headers.get("X-Rtunnel-Signature", ""),
            "ts": time.time(),
        })
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"ok":true}')

    def log_message(self, *a):
        pass


def wait_event(pred, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        for ev in list(Sink.events):
            if pred(ev):
                return ev
        time.sleep(0.2)
    return None


def req(method, path, body=None, cookie=None, port=None):
    url = "http://127.0.0.1:%d%s" % (port or ADMIN_PORT, path)
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        r.add_header("Content-Type", "application/json")
    if cookie:
        r.add_header("Cookie", cookie)
    try:
        with urllib.request.urlopen(r, timeout=10) as resp:
            return resp.status, dict(resp.headers), resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode()


ADMIN_PORT = 0


def main():
    global ADMIN_PORT
    os.makedirs(LOG, exist_ok=True)
    sink = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Sink)
    threading.Thread(target=sink.serve_forever, daemon=True).start()
    sink_url = "http://127.0.0.1:%d/hook" % sink.server_address[1]

    relay_port = e2e.free_port()
    ADMIN_PORT = e2e.free_port()
    echo_port = e2e.free_port()
    echo = e2e.EchoServer(echo_port)
    echo.start()

    certdir = os.path.join(LOG, "certs")
    os.makedirs(certdir, exist_ok=True)
    now_ms = int(time.time() * 1000)
    # 5 天后到期 → critical（crit=7）
    with open(os.path.join(certdir, "index.txt"), "w") as f:
        f.write("dev-old|device|7|%d|%d\n" % (now_ms - 86400000, now_ms + 5 * 86400000))

    log = open(os.path.join(LOG, "relay.log"), "w")
    relay = subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
         "--allow-all", "--drain-timeout", "300",
         "--admin", "127.0.0.1:%d" % ADMIN_PORT, "--admin-token", "admintok",
         "--admin-db", os.path.join(LOG, "admin.db"),
         "--admin-cert-dir", certdir],
        stdout=log, stderr=subprocess.STDOUT, env=ENV)
    agent = None
    try:
        time.sleep(1.0)
        st, _, _ = req("POST", "/api/v1/setup",
                       {"token": "admintok", "username": "root", "password": "pw123456"})
        e2e.check("alert: setup", st == 200, str(st))
        st, hdrs, _ = req("POST", "/api/v1/login", {"username": "root", "password": "pw123456"})
        cookie = hdrs.get("Set-Cookie", "").split(";")[0]
        e2e.check("alert: login", st == 200 and cookie.startswith("rt_sid="), str(st))

        settings = {
            "alert.enabled": "1",
            "alert.kind": "generic",
            "alert.webhook_url": sink_url,
            "alert.webhook_secret": "s3cr3t",
            "alert.device_offline": "1",
            "alert.offline_after_s": "30",
            "alert.cert_warn_days": "30",
            "alert.cert_crit_days": "7",
            "alert.task_fail": "1",
            "alert.cooldown_s": "10",
        }
        st, _, body = req("PUT", "/api/v1/settings", settings, cookie)
        e2e.check("alert: save settings", st == 200, body[:120])

        # 1) 通用 webhook + HMAC
        st, _, _ = req("POST", "/api/v1/alerts/test", {}, cookie)
        e2e.check("alert: test enqueued", st == 200, str(st))
        ev = wait_event(lambda e: b'"event":"test"' in e["body"], timeout=15)
        e2e.check("alert: generic webhook 收到测试", ev is not None)
        if ev:
            want = "sha256=" + hm.new(b"s3cr3t", ev["body"], hashlib.sha256).hexdigest()
            e2e.check("alert: HMAC 签名正确", ev["sig"] == want,
                      "%r != %r" % (ev["sig"], want))
            j = json.loads(ev["body"])
            e2e.check("alert: 载荷字段完整",
                      j.get("event") == "test" and j.get("level") == "info" and "relay" in j)

        st, _, body = req("GET", "/api/v1/alerts/status", None, cookie)
        js = json.loads(body)
        e2e.check("alert: status 显示已发送", js.get("sent_total", 0) >= 1, body[:160])
        e2e.check("alert: status 中 URL 脱敏",
                  js.get("url", "").endswith("/…hook") and sink_url not in body, js.get("url", ""))

        # 2) 证书到期（首次启用后 5s 内检查，先于会清空 Sink 的格式测试）
        ev = wait_event(lambda e: b"dev-old" in e["body"], timeout=25)
        e2e.check("alert: 证书到期告警", ev is not None)
        if ev:
            j = json.loads(ev["body"])
            e2e.check("alert: 到期级别 critical",
                      j.get("event") == "cert.expiring" and j.get("level") == "critical")

        # 3) 企业微信 / 钉钉 / 飞书格式
        for kind, marker in (("wecom", b'"msgtype":"markdown"'),
                             ("dingtalk", b'"msgtype":"markdown"'),
                             ("feishu", b'"msg_type":"text"')):
            Sink.events.clear()
            req("PUT", "/api/v1/settings", {"alert.kind": kind}, cookie)
            req("POST", "/api/v1/alerts/test", {}, cookie)
            ev = wait_event(lambda e: marker in e["body"], timeout=15)
            e2e.check("alert: %s 格式" % kind, ev is not None,
                      ev["body"][:120].decode(errors="ignore") if ev else "no request")

        req("PUT", "/api/v1/settings", {"alert.kind": "generic"}, cookie)

        # 4) 设备离线：起 agent，上线后杀掉，等 30s 阈值
        Sink.events.clear()
        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1:%d" % relay_port,
             "--devid", "dev-alert", "--allow", str(echo_port), "--verbose"],
            stdout=open(os.path.join(LOG, "agent.log"), "w"),
            stderr=subprocess.STDOUT, env=ENV)
        online = e2e.wait_log(os.path.join(LOG, "agent.log"), "authenticated as dev-alert", timeout=10)
        e2e.check("alert: agent 上线", online)
        time.sleep(6)          # 让 notifier 建基线
        agent.terminate()
        agent.wait(timeout=5)
        agent = None
        ev = wait_event(lambda e: b'"event":"device.offline"' in e["body"], timeout=50)
        e2e.check("alert: 设备离线告警", ev is not None)
        if ev:
            j = json.loads(ev["body"])
            e2e.check("alert: 离线事件字段",
                      j.get("details", {}).get("devid") == "dev-alert", ev["body"][:160].decode())
        extra = [e for e in Sink.events if b'"event":"device.offline"' in e["body"]]
        e2e.check("alert: 离线只发一次", len(extra) == 1, "count=%d" % len(extra))

        # 5) viewer 读设置：URL/secret 脱敏
        st, _, body = req("POST", "/api/v1/users",
                          {"username": "viewer1", "password": "pw123456", "role": "viewer"}, cookie)
        e2e.check("alert: 创建 viewer", st == 200, body[:120])
        st, hdrs, _ = req("POST", "/api/v1/login",
                          {"username": "viewer1", "password": "pw123456"})
        vck = hdrs.get("Set-Cookie", "").split(";")[0]
        st, _, body = req("GET", "/api/v1/settings", None, vck)
        js = json.loads(body)
        e2e.check("alert: viewer 看不到 secret", "alert.webhook_secret" not in js, body[:160])
        e2e.check("alert: viewer 看到脱敏 URL",
                  js.get("alert.webhook_url", "").endswith("/…hook"), js.get("alert.webhook_url", ""))
        e2e.check("alert: viewer 标记 _redacted", js.get("_redacted") is True)
    finally:
        if agent and agent.poll() is None:
            agent.terminate()
        relay.send_signal(15)
        try:
            relay.wait(timeout=8)
        except subprocess.TimeoutExpired:
            relay.kill()
        log.close()
        echo.close()
        sink.shutdown()
    return e2e.finish()


if __name__ == "__main__":
    sys.exit(main())
