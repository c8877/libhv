#!/usr/bin/env python3
"""多租户隔离 e2e：设备归属租户后，接入令牌只能访问本租户设备。

场景：
  1. dev1 默认在租户 1；租户 2 的令牌无法连 dev1
  2. 把 dev1 划到租户 2 → 租户 2 的令牌可连，租户 1 的令牌被拒
  3. 租户 2 令牌的设备列表只看得到本租户设备
  4. 停用租户 2 → 其令牌认证被拒
  5. 用量按租户归集，tenant_id 过滤生效
"""

import json
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
LOG = "/tmp/rtunnel-tenant"


def new_account(call, username, tenant_id):
    try:
        return call("POST", "/api/v1/client-accounts",
                    {"username": username, "display_name": username, "tenant_id": tenant_id})
    except urllib.error.HTTPError as e:
        if e.code != 400:
            raise
        lst = call("GET", "/api/v1/client-accounts")
        items = lst if isinstance(lst, list) else lst.get("items", [])
        return next(a for a in items if a.get("username") == username)


def add_group(call, name, account_id, all_devices=True, devices=None, resources=None):
    return call("POST", "/api/v1/permission/groups", {
        "name": name, "description": "tenant-e2e",
        "resources": resources if resources is not None else e2e.E2E_RES,
        "all_devices": all_devices, "devices": devices or [],
        "ports": "*", "members": [{"type": "client", "id": str(account_id)}],
    })


def socks_roundtrip(socks_ep, echo_port, timeout):
    try:
        s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=3)
    except OSError as e:
        return False, str(e)
    if s is None:
        return False, "socks rep=%d" % rep
    try:
        s.sendall(b"ping")
        got = e2e.recv_exact(s, 4)
        return got == b"ping", "echo=%r" % got
    except OSError as e:
        return False, str(e)
    finally:
        s.close()


def bearer_get(base, path, token):
    req = urllib.request.Request(base + path, headers={"Authorization": "Bearer " + token})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode() or "{}")


def bearer_call(base, path, token):
    req = urllib.request.Request(base + path, headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return e.code, {}


def bearer_post(base, path, token, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return e.code, {}


def wait_roundtrip(socks_ep, echo_port, want, timeout):
    """want=True 等通，want=False 等被拒；返回 (符合预期, 详情)。"""
    end = time.time() + timeout
    last = ""
    while time.time() < end:
        ok, detail = socks_roundtrip(socks_ep, echo_port, timeout=2)
        last = detail
        if ok == want:
            return True, detail
        time.sleep(0.3)
    return False, last


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

        # 租户 2 及其接入账号
        t2 = call("POST", "/api/v1/tenants", {"name": "租户二", "note": "e2e"})
        e2e.check("tenant: 创建租户", t2.get("ok") is True and t2.get("id", 0) > 0, str(t2))
        tid = t2["id"]

        acc2 = new_account(call, "t2user", tid)
        add_group(call, "t2-all", acc2["id"],
                  resources=e2e.E2E_RES + ["account.read", "account.write",
                                           "group.read", "dict.read", "token.read"])
        tok2 = call("POST", "/api/v1/client-accounts/%d/tokens" % acc2["id"], {"name": "t2"})["token"]

        acc1 = new_account(call, "t1user", 1)
        add_group(call, "t1-all", acc1["id"])
        tok1 = call("POST", "/api/v1/client-accounts/%d/tokens" % acc1["id"], {"name": "t1"})["token"]

        def start_client(tok):
            p = subprocess.Popen(
                [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
                 "--token", tok, "--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
                stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
            procs.append(p)
            return p

        # 1) dev1 在默认租户：租户 2 的令牌连不上，租户 1 可以
        c1 = start_client(tok1)
        ok, detail = wait_roundtrip(socks_ep, echo_port, True, 15)
        e2e.check("tenant: 同租户可连 dev1", ok, detail)
        c1.send_signal(15)
        c1.wait(timeout=5)
        c2 = start_client(tok2)
        ok, detail = wait_roundtrip(socks_ep, echo_port, False, 6)
        e2e.check("tenant: 跨租户被拒", ok, detail)

        # 1.5) 接入令牌不能管理租户，也不能跨租户建号
        code, _ = bearer_post(base, "/api/v1/tenants", tok2, {"name": "偷建"})
        e2e.check("tenant: 接入令牌不能建租户", code == 403, "status=%d" % code)
        code, acc_new = bearer_post(base, "/api/v1/client-accounts", tok2,
                                    {"username": "sneak", "tenant_id": 1})
        got_tenant = 0
        if code == 200:
            det = bearer_get(base, "/api/v1/client-accounts/%d" % acc_new.get("id", 0), tok2)
            got_tenant = det.get("tenant_id", 0)
        e2e.check("tenant: 建号被限制在自租户", code == 200 and got_tenant == tid,
                  "status=%d tenant=%s" % (code, got_tenant))

        # 1.6) 跨租户读账号/权限组被拒；账号列表只看本租户；授权对象里没有控制台账号
        code, _ = bearer_call(base, "/api/v1/client-accounts/%d" % acc1["id"], tok2)
        e2e.check("tenant: 不能读别的租户账号", code == 403, "status=%d" % code)
        code, _ = bearer_call(base, "/api/v1/client-accounts/%d/perm-groups" % acc1["id"], tok2)
        e2e.check("tenant: 不能改别的租户账号的组", code == 403, "status=%d" % code)
        code, lst = bearer_call(base, "/api/v1/client-accounts", tok2)
        names = [a.get("username") for a in (lst if isinstance(lst, list) else lst.get("items", []))]
        e2e.check("tenant: 账号列表只看本租户", "t1user" not in names, str(names))
        code, subj = bearer_call(base, "/api/v1/permission/subjects", tok2)
        items = subj if isinstance(subj, list) else subj.get("items", [])
        types = {x.get("type") for x in items}
        e2e.check("tenant: 授权对象不含控制台账号", "user" not in types and "atk" not in types, str(types))
        # 运营商侧资源（权限组/字典/atk）即使授给接入令牌也不生效
        code, _ = bearer_call(base, "/api/v1/permission/groups", tok2)
        e2e.check("tenant: 接入令牌不能读权限组", code == 403, "status=%d" % code)
        code, _ = bearer_call(base, "/api/v1/permission/dicts?kind=devices", tok2)
        e2e.check("tenant: 接入令牌不能读字典", code == 403, "status=%d" % code)
        code, _ = bearer_call(base, "/api/v1/admin-tokens", tok2)
        e2e.check("tenant: 接入令牌不能读 atk 列表", code == 403, "status=%d" % code)

        # 2) 设备划到租户 2
        call("PUT", "/api/v1/permission/devices/dev1", {"name": "", "enabled": True, "tenant_id": tid})
        ok, detail = wait_roundtrip(socks_ep, echo_port, True, 10)
        e2e.check("tenant: 设备迁移后本租户可连", ok, detail)

        # 3) 设备列表按租户过滤：租户 2 的令牌看得到 dev1，看不到别的租户设备
        devs = bearer_get(base, "/api/v1/agents", tok2)
        items = devs if isinstance(devs, list) else devs.get("items", [])
        ids = [d.get("devid") for d in items]
        e2e.check("tenant: 租户 2 令牌只看本租户设备", "dev1" in ids, str(ids))

        # 4) 停用租户 → 令牌认证被拒（relay 5s 内下发策略并断开旧连接）
        call("PUT", "/api/v1/tenants/%d" % tid, {"name": "租户二", "status": "disabled"})
        blocked, last = False, ""
        end = time.time() + 15
        while time.time() < end:
            try:
                s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=3)
            except OSError as e:
                blocked, last = True, str(e)
                break
            if s is None:
                blocked, last = True, "rep=%d" % rep
                break
            last = "still open"
            s.close()
            time.sleep(0.5)
        e2e.check("tenant: 停用租户后无法再建会话", blocked, last)

        # 5) 用量按租户归集
        time.sleep(6.5)
        rows = call("GET", "/api/v1/usage?metric=sessions&tenant_id=%d" % tid).get("rows", [])
        e2e.check("tenant: 用量可按租户过滤", all(r.get("tenant_id") == tid for r in rows) and len(rows) > 0,
                  "rows=%d" % len(rows))

        db = sqlite3.connect(os.path.join(LOG, "admin.db"))
        ev = db.execute("SELECT COUNT(*) FROM usage_events WHERE tenant_id=?", (tid,)).fetchone()[0]
        db.close()
        e2e.check("tenant: 流水带 tenant_id", ev > 0, "events=%d" % ev)
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
