#!/usr/bin/env python3
"""接入账号与 Token 的端到端用例（自启 relay / agent / echo，跑完自动清理）。

覆盖：
  UI     新建账号 → 新建 Token（一次性明文）→ 展开 → 轮换 → 停用 → 有效权限 → 删除
  认证   Token 单独登录（省略 --user）、用户名不匹配、无效 Token 停止重连
  限额   请求频率、并发会话、流量配额（cut）、停用踢线、用量落库

用法：
  python3 scripts/admin_token_e2e.py           # 自启环境，跑完清理
  python3 scripts/admin_token_e2e.py --keep    # 保留数据库、日志与截图
依赖：playwright（python3 -m pip install playwright && playwright install chromium）
"""
import argparse
import http.cookiejar
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "rtunnel", "tests"))
from e2e import EchoServer, free_port, socks5_open  # noqa: E402

BIN = os.path.join(ROOT, "build", "bin")


class Runner:
    def __init__(self, work):
        self.work = work
        self.procs = []
        self.fails = []

    def check(self, name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  (" + str(detail)[:200] + ")"))
        if not ok:
            self.fails.append(name)

    def start(self, cmd, log):
        p = subprocess.Popen(cmd, stdout=open(os.path.join(self.work, log), "w"),
                             stderr=subprocess.STDOUT, start_new_session=True)
        self.procs.append(p)
        return p

    def stop(self):
        for p in reversed(self.procs):
            try:
                p.terminate()
                p.wait(timeout=3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


def make_api(base):
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    def call(method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(base + path, data=data, method=method)
        if data:
            req.add_header("Content-Type", "application/json")
        try:
            with opener.open(req, timeout=5) as r:
                return json.loads(r.read().decode() or "{}")
        except urllib.error.HTTPError as e:
            return {"_code": e.code, "_body": e.read().decode()}

    return call


def socks_ping(port, devid, target, payload=b"ping"):
    s, rep = socks5_open(("tcp", "127.0.0.1", port), devid, target, timeout=5)
    if s is None:
        return False, rep
    try:
        s.sendall(payload)
        got = s.recv(len(payload))
        return got == payload, rep
    except OSError:
        return False, rep
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="token-ui-")
    r = Runner(work)
    rp, admin_port = free_port(), free_port()
    e1, e2 = free_port(), free_port()
    echo1, echo2 = EchoServer(e1), EchoServer(e2)
    echo1.start()
    echo2.start()
    base = "http://127.0.0.1:%d" % admin_port

    r.start([BIN + "/rtunnel-relay", "--host", "127.0.0.1", "--port", str(rp),
             "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "boot",
             "--admin-db", os.path.join(work, "admin.db"),
             "--admin-file-dir", os.path.join(work, "files")], "relay.log")
    time.sleep(1.0)

    api = make_api(base)
    api("POST", "/api/v1/setup", {"token": "boot", "username": "root", "password": "rootpass1"})
    api("POST", "/api/v1/login", {"username": "root", "password": "rootpass1"})

    caps = {"tcp_ports": "*", "shell": False, "tty": "", "file": "none",
            "sysinfo": False, "metrics": False, "config": False, "web": False}
    gid = api("POST", "/api/v1/permission/agent-groups",
              {"name": "g", "description": "", "devices": ["dev1"], "caps": caps})["id"]
    acl = dict(caps, agent_group_id=gid)
    api("POST", "/api/v1/permission/client-groups",
        {"name": "c", "description": "", "users": ["alice"], "acl": [acl]})

    r.start([BIN + "/rtunnel-agent", "--server", "127.0.0.1", "--port", str(rp),
             "--devid", "dev1", "--allow", "%d,%d" % (e1, e2),
             "--config-file", os.path.join(work, "agent.json")], "agent.log")
    time.sleep(1.2)

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("需要 playwright：python3 -m pip install playwright && playwright install chromium")
        r.stop()
        return 2

    shots = os.path.join(work, "shots")
    os.makedirs(shots, exist_ok=True)
    tokens = {}

    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page(viewport={"width": 1500, "height": 950})
        errs = []
        page.on("pageerror", lambda e: errs.append(str(e)))

        # ---------------------------------------------------------- UI
        page.goto(base, wait_until="networkidle")
        if page.locator("#af-token").count():
            page.fill("#af-token", "boot")
        page.fill("#af-user", "root")
        page.fill("#af-pass", "rootpass1")
        page.click("#af-btn")
        page.wait_for_timeout(900)
        r.check("1 登录", page.evaluate("!!document.getElementById('nav')"))

        page.goto(base + "/#/access")
        page.wait_for_timeout(700)
        nav = page.evaluate("[...document.querySelectorAll('#nav a')].map(x=>x.textContent).join(' ')")
        r.check("2 导航含接入账号", "接入账号" in nav, nav)
        r.check("3 空列表", "暂无接入账号" in page.evaluate("document.querySelector('#page').innerText"))

        page.locator("#acc-new").click()
        page.wait_for_timeout(400)
        page.fill("#aa-user", "alice")
        page.fill("#aa-display", "Alice")
        page.locator("#aa-ok").click()
        page.wait_for_timeout(900)
        r.check("4 新建账号", "alice" in page.evaluate("document.querySelector('#page').innerText"))

        page.locator(".acc-row").first.click()
        page.wait_for_timeout(800)
        account_url = page.url
        r.check("5 进入账号详情", page.locator(".acc-kv").count() == 1 and "#/access/" in account_url,
                account_url)

        page.locator("[data-act=tok-new]").click()
        page.wait_for_timeout(400)
        page.fill("#at-name", "限速")
        page.select_option("#at-expire", "30")
        page.fill("#at-quota", "1")
        page.select_option("#at-period", "monthly")
        page.fill("#at-rate", "2")
        page.fill("#at-burst", "2")
        page.locator("#at-ok").click()
        page.wait_for_timeout(900)
        secret = page.evaluate("document.getElementById('as-value')?.textContent||''")
        r.check("6 一次性明文", secret.startswith("rtk_") and len(secret) == 47, secret)
        tokens["limit"] = secret
        page.locator("[data-modal-close]").first.click()
        page.wait_for_timeout(900)
        r.check("7 详情出现 Token", "限速" in page.evaluate("document.querySelector('.acc-token-table').innerText"))

        page.reload(wait_until="networkidle")
        page.wait_for_timeout(800)
        row = page.evaluate("document.querySelector('.acc-token-table tbody tr')?.innerText||''")
        r.check("8 刷新后回读", "限速" in row, row.replace("\n", " | "))

        page.locator("[data-act=acc-effective]").click()
        page.wait_for_timeout(600)
        eff = page.evaluate("document.querySelector('.acc-eff')?.innerText||''")
        r.check("9 有效权限", "所属客户端分组" in eff and "c" in eff, eff[:120])
        page.locator("[data-modal-close]").first.click()
        page.wait_for_timeout(300)
        page.screenshot(path=os.path.join(shots, "access-page.png"), full_page=True)

        # 再建一个不限速的 Token 用于运行时用例
        page.locator("[data-act=tok-new]").click()
        page.wait_for_timeout(400)
        page.fill("#at-name", "常规")
        page.select_option("#at-expire", "90")
        page.locator("#at-ok").click()
        page.wait_for_timeout(900)
        tokens["normal"] = page.evaluate("document.getElementById('as-value')?.textContent||''")
        page.locator("[data-modal-close]").first.click()
        page.wait_for_timeout(600)
        r.check("10 第二个 Token", tokens["normal"].startswith("rtk_"))

        # 行内展开表单：只读行保留，改完立即回读
        page.locator("[data-act=tok-edit]").first.click()
        page.wait_for_timeout(400)
        r.check("11 行内表单展开", page.locator("tr.tok-edit-row .tok-form").count() == 1 and
                page.locator("tr.tok-row").count() == 2)
        page.fill("#te-name", "常规-改")
        page.fill("#te-quota", "2")
        page.fill("#te-rate", "5")
        page.fill("#te-sessions", "3")
        page.locator("[data-act=tok-save]").click()
        page.wait_for_timeout(900)
        edited = page.evaluate("document.querySelector('.acc-token-table tbody tr')?.innerText||''")
        r.check("12 行内表单保存", "常规-改" in edited and "2.0 GB" in edited and
                "5/分" in edited and "并发 3" in edited, edited.replace("\n", " | "))

        # 状态筛选
        page.locator("#tok-tabs .ui-tab[data-key=disabled]").click()
        page.wait_for_timeout(400)
        r.check("13 状态筛选", "暂无 Token" in page.evaluate("document.querySelector('.acc-token-table').innerText"))
        page.locator("#tok-tabs .ui-tab[data-key=all]").click()
        page.wait_for_timeout(400)

        # 轮换「限速」那一行（列表按 id 倒序，第二行）
        page.locator("[data-act=tok-rotate]").nth(1).click()
        page.wait_for_timeout(400)
        page.locator(".tty-btns button").last.click()
        page.wait_for_timeout(800)
        rotated = page.evaluate("document.getElementById('as-value')?.textContent||''")
        r.check("14 轮换生成新 Token", rotated.startswith("rtk_") and rotated not in tokens.values(), rotated[:16])
        if rotated.startswith("rtk_"):
            tokens["limit"] = rotated
        page.locator("[data-modal-close]").first.click()
        page.wait_for_timeout(600)

        # 不存在的账号
        page.goto(base + "/#/access/9999")
        page.wait_for_timeout(700)
        r.check("15 详情 404", "账号不存在" in page.evaluate("document.querySelector('#page').innerText"))

        page.goto(account_url)
        page.wait_for_timeout(700)
        page.screenshot(path=os.path.join(shots, "access-tokens.png"), full_page=True)
        browser.close()

    # ---------------------------------------------------------- 运行时
    def try_open(port, devid="dev1", target=e1):
        """开一次连接后立即关闭，只返回 SOCKS 应答码。"""
        s, rep = socks5_open(("tcp", "127.0.0.1", port), devid, target, timeout=5)
        if s:
            s.close()
        return rep

    sp = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--token", tokens["normal"], "--socks5", "127.0.0.1:%d" % sp, "--verbose"], "client.log")
    time.sleep(1.4)
    log = open(os.path.join(work, "client.log")).read()
    r.check("16 Token 单独登录", "authenticated as alice" in log, log[-160:])
    ok, rep = socks_ping(sp, "dev1", e1)
    r.check("17 Token 授权可访问设备", ok, "rep=%d" % rep)

    # 请求频率：2 次/分，burst 2
    sp_lim = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--token", tokens["limit"], "--socks5", "127.0.0.1:%d" % sp_lim, "--verbose"], "limit.log")
    time.sleep(1.4)
    reps = [try_open(sp_lim) for _ in range(3)]
    r.check("18 请求频率：前两次放行", reps[0] == 0 and reps[1] == 0, reps)
    r.check("19 请求频率：第三次被限", reps[2] != 0, reps)

    # 并发会话：max_sessions=1
    res = api("POST", "/api/v1/client-accounts/%d/tokens" % 1,
              {"name": "并发", "max_sessions": 1})
    sp_ses = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--token", res.get("token", ""), "--socks5", "127.0.0.1:%d" % sp_ses, "--verbose"], "session.log")
    time.sleep(1.4)
    s1, p1 = socks5_open(("tcp", "127.0.0.1", sp_ses), "dev1", e1, timeout=5)
    p2 = try_open(sp_ses)
    r.check("20 并发：第一条放行", s1 is not None and p1 == 0, "rep=%d" % p1)
    r.check("21 并发：第二条被拒", p2 != 0, "rep=%d" % p2)
    if s1:
        s1.close()

    # 无效 Token：停止重连
    sp_bad = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--token", "rtk_" + "A" * 43, "--socks5", "127.0.0.1:%d" % sp_bad, "--verbose"], "bad.log")
    time.sleep(1.4)
    bad = open(os.path.join(work, "bad.log")).read()
    r.check("22 无效 Token 停止重连", "stop reconnecting" in bad, bad[-160:])

    # 用户名与 Token 不符
    sp_mis = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--user", "bob", "--token", tokens["normal"],
             "--socks5", "127.0.0.1:%d" % sp_mis, "--verbose"], "mismatch.log")
    time.sleep(1.4)
    mis = open(os.path.join(work, "mismatch.log")).read()
    r.check("23 用户名不符被拒", "user mismatch" in mis, mis[-160:])

    # 停用 Token：在线连接被踢、重连被拒
    toks = api("GET", "/api/v1/client-accounts/1/tokens")
    normal = [t for t in toks if t["name"] == "常规-改"][0]
    api("PUT", "/api/v1/client-tokens/%d" % normal["id"], {"status": "disabled"})
    time.sleep(1.0)
    ok2, rep2 = socks_ping(sp, "dev1", e1)
    r.check("24 停用 Token 后拒绝", not ok2, "rep=%d" % rep2)

    # 流量配额：32 字节，cut
    api("POST", "/api/v1/client-accounts", {"username": "carol"})
    api("PUT", "/api/v1/permission/client-groups/1",
        {"name": "c", "description": "", "users": ["alice", "carol"], "acl": [acl]})
    carol = api("GET", "/api/v1/client-accounts?q=carol")[0]
    res = api("POST", "/api/v1/client-accounts/%d/tokens" % carol["id"],
              {"name": "配额", "quota_bytes": 32, "quota_action": "cut"})
    sp_q = free_port()
    r.start([BIN + "/rtunnel-client", "--server", "127.0.0.1", "--port", str(rp),
             "--token", res.get("token", ""), "--socks5", "127.0.0.1:%d" % sp_q, "--verbose"], "quota.log")
    time.sleep(1.4)
    s, rep3 = socks5_open(("tcp", "127.0.0.1", sp_q), "dev1", e1, timeout=5)
    inside = False
    if s:
        try:
            s.sendall(b"12345678")
            inside = s.recv(8) == b"12345678"
            s.sendall(b"x" * 24)
            s.recv(24)
        except OSError:
            pass
        s.close()
    r.check("25 配额内可收发", inside, "rep=%d" % rep3)
    time.sleep(1.0)
    p4 = try_open(sp_q)
    r.check("26 超配额后拒绝", p4 != 0, "rep=%d" % p4)

    # 用量落库
    time.sleep(6)
    q = subprocess.check_output(
        ["sqlite3", os.path.join(work, "admin.db"),
         "select used_bytes from client_tokens where name='配额';"], text=True).strip()
    r.check("27 用量已落库", bool(q) and int(q) >= 32, q)

    r.check("28 全程无 JS 报错", not errs, errs[:3])

    r.stop()
    echo1.close()
    echo2.close()

    total = 28
    print("=== %d/%d passed === work=%s" % (total - len(r.fails), total, work))
    if args.keep:
        print("保留：", work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
