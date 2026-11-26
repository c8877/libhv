#!/usr/bin/env python3
"""AI 会话记录：落库、历史切换、最近 10 个 + 翻页、永久保留、账号隔离。

不需要真实模型：起一个最小 OpenAI 兼容服务，永远只回一句话。
重点是会话本身的行为，不是模型能力。
"""

import http.cookiejar
import json
import os
import shutil
import subprocess
import threading
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-ai-session"


class Echo(BaseHTTPRequestHandler):
    """最小假模型：不调工具，直接把最后一条 user 消息回显一半。"""

    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n).decode() or "{}")
        last = ""
        for m in body.get("messages", []):
            if m.get("role") == "user":
                last = m.get("content", "")
        out = json.dumps({
            "model": body.get("model", ""),
            "choices": [{"message": {"role": "assistant", "content": "收到：" + last[:40]},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


def call_json(op, base, path, body=None, method="POST"):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with op.open(req, timeout=30) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def login(base, user, password):
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    req = urllib.request.Request(base + "/api/v1/login",
                                data=json.dumps({"username": user, "password": password}).encode(),
                                method="POST")
    req.add_header("Content-Type", "application/json")
    op.open(req, timeout=5).read()
    return op


def start_relay(port, admin_port, db):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    log = open(os.path.join(LOG, "relay.log"), "a")
    p = subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(port),
         "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "admintok",
         "--admin-db", db, "--admin-file-dir", os.path.join(LOG, "files")],
        stdout=log, stderr=subprocess.STDOUT, env=env)
    return p, log


def ask(op, base, text, session_id=0):
    return call_json(op, base, "/api/v1/ai/chat",
                     {"messages": [{"role": "user", "content": text}],
                      **({"session_id": session_id} if session_id else {})})


def main():
    if os.path.isdir(LOG):
        shutil.rmtree(LOG, ignore_errors=True)
    os.makedirs(LOG, exist_ok=True)

    model = ThreadingHTTPServer(("127.0.0.1", 0), Echo)
    model_port = model.server_address[1]
    threading.Thread(target=model.serve_forever, daemon=True).start()

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    db = os.path.join(LOG, "admin.db")
    relay, rlog = start_relay(relay_port, admin_port, db)
    base = "http://127.0.0.1:%d" % admin_port
    adm = None
    for _ in range(60):
        try:
            adm, _ = e2e.admin_client(base)
            break
        except Exception:
            time.sleep(0.25)
    if adm is None:
        print("relay 未就绪")
        sys.exit(1)

    try:
        # 配好模型（只配这一个账号）
        code, _ = call_json(adm, base, "/api/v1/ai/config", {
            "endpoint": "http://127.0.0.1:%d/v1" % model_port,
            "model": "echo-model",
        }, method="PUT")
        e2e.check("sess: 保存模型配置", code == 200, "code=%d" % code)
        code, _ = call_json(adm, base, "/api/v1/ai/key", {"key": "sk-session-123456"}, method="PUT")
        e2e.check("sess: 保存密钥", code == 200, "code=%d" % code)

        # 1) 不带 session_id：自动建会话，标题取问句
        code, r1 = ask(adm, base, "第一条会话的问题是什么")
        sid = r1.get("session_id", 0)
        e2e.check("sess: 首次提问自动建会话", code == 200 and sid > 0,
                  "code=%d r=%s" % (code, str(r1)[:160]))

        code, lst = call_json(adm, base, "/api/v1/ai/sessions", None, method="GET")
        items = lst.get("items", [])
        e2e.check("sess: 列表出现该会话", code == 200 and len(items) == 1 and items[0]["id"] == sid,
                  "code=%d items=%s" % (code, items))
        e2e.check("sess: 标题取问句", items and items[0]["title"] == "第一条会话的问题是什么",
                  str(items[0]["title"]) if items else "-")
        e2e.check("sess: 一轮问答计 2 条消息", items and items[0]["msg_count"] == 2,
                  "msg_count=%s" % (items[0]["msg_count"] if items else "-"))

        # 2) 带 session_id：续在同一会话
        code, r2 = ask(adm, base, "第二个问题", sid)
        e2e.check("sess: 续问返回同一会话", code == 200 and r2.get("session_id") == sid,
                  "sid=%s vs %s" % (r2.get("session_id"), sid))
        code, lst = call_json(adm, base, "/api/v1/ai/sessions", None, method="GET")
        items = lst.get("items", [])
        e2e.check("sess: 消息数累加到 4", items and items[0]["msg_count"] == 4,
                  "msg_count=%s" % (items[0]["msg_count"] if items else "-"))

        # 3) 详情：顺序与内容
        code, det = call_json(adm, base, "/api/v1/ai/sessions/%d" % sid, None, method="GET")
        msgs = det.get("messages", [])
        e2e.check("sess: 详情返回 4 条消息", code == 200 and len(msgs) == 4, "n=%d" % len(msgs))
        e2e.check("sess: 消息顺序正确",
                  [m.get("role") for m in msgs] == ["user", "assistant", "user", "assistant"],
                  str([m.get("role") for m in msgs]))
        e2e.check("sess: 首条内容正确", msgs and msgs[0].get("content") == "第一条会话的问题是什么",
                  msgs[0].get("content") if msgs else "-")
        e2e.check("sess: 回复也存下来了",
                  len(msgs) > 1 and msgs[1].get("content", "").startswith("收到："),
                  msgs[1].get("content") if len(msgs) > 1 else "-")
        e2e.check("sess: assistant 带 meta", len(msgs) > 1 and "meta" in msgs[1],
                  str(msgs[1].keys()) if len(msgs) > 1 else "-")

        # 4) 再建 11 个会话 → 列表默认只回 10 个，且有翻页游标
        for k in range(11):
            code, rk = ask(adm, base, "批量会话 %d" % k)
            if code != 200:
                e2e.check("sess: 批量建会话失败", False, "k=%d code=%d" % (k, code))
                break
            time.sleep(0.01)   # 让 updated_ms 有区分度
        code, p1 = call_json(adm, base, "/api/v1/ai/sessions", None, method="GET")
        items1 = p1.get("items", [])
        e2e.check("sess: 默认只回最近 10 个", code == 200 and len(items1) == 10, "n=%d" % len(items1))
        e2e.check("sess: 标记还有更多", p1.get("has_more") is True, str(p1.get("has_more")))
        e2e.check("sess: 给翻页游标", p1.get("next_before_ms", 0) > 0, str(p1.get("next_before_ms")))
        e2e.check("sess: 最新在前", bool(items1) and items1[0]["msg_count"] == 2,
                  str(items1[0]) if items1 else "-")

        before = p1.get("next_before_ms", 0)
        before_id = p1.get("next_before_id", 0)
        code, p2 = call_json(adm, base, "/api/v1/ai/sessions?before=%d&before_id=%d"
                             % (before, before_id), None, method="GET")
        items2 = p2.get("items", [])
        e2e.check("sess: 翻页拿到更早的", code == 200 and len(items2) >= 2,
                  "n=%d p1=%d before=%d/%d ids=%s" % (len(items2), len(items1), before, before_id,
                                                      [x["id"] for x in items1]))
        ids1 = {x["id"] for x in items1}
        e2e.check("sess: 两页不重叠", all(x["id"] not in ids1 for x in items2),
                  str([x["id"] for x in items2]))

        # 5) 改名
        code, rr = call_json(adm, base, "/api/v1/ai/sessions/%d" % sid, {"title": "改过的标题"},
                             method="PUT")
        e2e.check("sess: 改名成功", code == 200 and rr.get("title") == "改过的标题", str(rr))

        # 6) 账号隔离：另一个账号看不到也读不到
        call_json(adm, base, "/api/v1/users",
                  {"username": "sessother", "password": "sessother1", "role": "manager"})
        other = login(base, "sessother", "sessother1")
        call_json(other, base, "/api/v1/ai/config", {
            "endpoint": "http://127.0.0.1:%d/v1" % model_port, "model": "echo-model",
        }, method="PUT")
        call_json(other, base, "/api/v1/ai/key", {"key": "sk-other-123456"}, method="PUT")
        code, olst = call_json(other, base, "/api/v1/ai/sessions", None, method="GET")
        e2e.check("sess: 他人列表为空", code == 200 and len(olst.get("items", [])) == 0,
                  str(olst)[:120])
        code, _ = call_json(other, base, "/api/v1/ai/sessions/%d" % sid, None, method="GET")
        e2e.check("sess: 读不到他人会话", code == 404, "code=%d" % code)
        code, _ = call_json(other, base, "/api/v1/ai/sessions/%d" % sid, {"title": "x"},
                            method="PUT")
        e2e.check("sess: 改不了他人会话", code == 404, "code=%d" % code)
        code, _ = call_json(other, base, "/api/v1/ai/sessions/%d" % sid, None, method="DELETE")
        e2e.check("sess: 删不了他人会话", code == 404, "code=%d" % code)
        code, r = ask(other, base, "别人的会话不该被续上", sid)
        e2e.check("sess: 带他人 session_id 提问另建会话",
                  code == 200 and r.get("session_id") not in (0, sid),
                  "sid=%s got=%s" % (sid, r.get("session_id")))
        code, o2 = call_json(other, base, "/api/v1/ai/sessions", None, method="GET")
        e2e.check("sess: 他人自己的会话正常出现",
                  code == 200 and len(o2.get("items", [])) == 1, str(o2)[:160])

        # 7) 失败对话也留痕
        call_json(adm, base, "/api/v1/ai/config", {"model": "will-not-fail"}, method="PUT")
        model.shutdown()
        model.server_close()
        code, rf = ask(adm, base, "这条会失败")
        e2e.check("sess: 失败返回 502", code == 502, "code=%d" % code)
        fsid = rf.get("session_id", 0)
        e2e.check("sess: 失败也建了会话", fsid > 0, "sid=%s" % fsid)
        if fsid:
            code, fdet = call_json(adm, base, "/api/v1/ai/sessions/%d" % fsid, None, method="GET")
            fm = fdet.get("messages", [])
            e2e.check("sess: 失败记录含 error",
                      len(fm) == 2 and fm[1].get("role") == "assistant" and fm[1].get("error"),
                      str(fm)[:200])
            code, _ = call_json(adm, base, "/api/v1/ai/sessions/%d" % fsid, None, method="DELETE")
            e2e.check("sess: 删除会话成功", code == 200, "code=%d" % code)
            code, _ = call_json(adm, base, "/api/v1/ai/sessions/%d" % fsid, None, method="GET")
            e2e.check("sess: 删除后读不到", code == 404, "code=%d" % code)

        # 8) 永久保留：重启 relay（同一份库）后会话还在
        count_before = len(items1) + len(items2)
        relay.send_signal(15)
        relay.wait(timeout=10)
        rlog.close()
        relay, rlog = start_relay(relay_port, admin_port, db)
        adm2 = None
        for _ in range(60):
            try:
                adm2, _ = e2e.admin_client(base)
                break
            except Exception:
                time.sleep(0.25)
        if adm2 is None:
            e2e.check("sess: 重启后能登录", False, "relay not ready")
            sys.exit(1)
        code, after = call_json(adm2, base, "/api/v1/ai/sessions?limit=50", None, method="GET")
        e2e.check("sess: 重启后会话仍在", code == 200 and len(after.get("items", [])) == count_before,
                  "before=%d after=%d" % (count_before, len(after.get("items", []))))
        code, det2 = call_json(adm2, base, "/api/v1/ai/sessions/%d" % sid, None, method="GET")
        e2e.check("sess: 重启后消息完整",
                  code == 200 and len(det2.get("messages", [])) == 4 and
                  det2.get("title") == "改过的标题",
                  "code=%d n=%d title=%s" % (code, len(det2.get("messages", [])),
                                             det2.get("title")))

        # 9) 未登录
        bare = urllib.request.build_opener()
        code, _ = call_json(bare, base, "/api/v1/ai/sessions", None, method="GET")
        e2e.check("sess: 未登录被拒", code in (401, 403), "code=%d" % code)
    finally:
        try:
            relay.send_signal(15)
            relay.wait(timeout=5)
        except Exception:
            try:
                relay.kill()
            except Exception:
                pass
        rlog.close()
        try:
            model.shutdown()
        except Exception:
            pass


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
