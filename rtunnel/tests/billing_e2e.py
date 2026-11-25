#!/usr/bin/env python3
"""费用试算 e2e：价目设置 + 用量 × 单价，按租户汇总。

场景：
  1. 先跑一条会话（流量 + 会话次数）
  2. 设置会话次数 0.01 元/次、流量 0.5 元/MiB
  3. summary 里 sessions 金额 = 次数 × 0.01 元，bytes 金额合理
  4. 价目表回读；未知指标被拒
  5. 接入令牌只算本租户
"""

import os
import subprocess
import sys
import time
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-billing"


def today():
    return time.strftime("%Y-%m-%d")


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
        time.sleep(0.8)

        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)

        tok = e2e.create_client_token(base, "bill")
        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--token", tok, "--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
        procs.append(client)

        # 等连通并跑一条会话
        s = None
        for _ in range(60):
            try:
                s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=2)
            except OSError:
                s = None
            if s is not None:
                break
            time.sleep(0.2)
        e2e.check("billing: 基线连通", s is not None, "socks failed")
        if s is not None:
            payload = b"m" * 4096
            s.sendall(payload)
            e2e.recv_exact(s, len(payload))
            s.close()

        # 等管理面把用量落库
        time.sleep(6.5)
        _, call = e2e.admin_client(base)

        body = {"unit_size": 1, "price_yuan": 0.01}
        call("PUT", "/api/v1/billing/prices/sessions", body)
        call("PUT", "/api/v1/billing/prices/bytes",
             {"unit_size": 1048576, "price_yuan": 0.5})

        prices = call("GET", "/api/v1/billing/prices").get("items", [])
        by = {p["metric"]: p for p in prices}
        e2e.check("billing: 价目已保存", by.get("sessions", {}).get("price_micro") == 10000,
                  str(by.get("sessions")))
        e2e.check("billing: 流量价目 0.5 元/MiB",
                  by.get("bytes", {}).get("price_micro") == 500000 and
                  by.get("bytes", {}).get("unit_size") == 1048576, str(by.get("bytes")))

        # 未知指标被拒
        code = 0
        try:
            call("PUT", "/api/v1/billing/prices/foo", {"unit_size": 1, "price_yuan": 1})
        except urllib.error.HTTPError as e:
            code = e.code
        e2e.check("billing: 未知指标被拒", code == 400, "status=%d" % code)

        day = today()
        summary = call("GET", "/api/v1/billing/summary?from=%s&to=%s" % (day, day))
        tenants = summary.get("tenants", [])
        e2e.check("billing: 有租户汇总", len(tenants) >= 1, str(tenants)[:200])
        items = {}
        if tenants:
            for it in tenants[0].get("items", []):
                items[it["metric"]] = it
        sess = items.get("sessions", {})
        e2e.check("billing: 会话金额 = 次数 × 单价",
                  sess.get("qty", 0) >= 1 and sess.get("amount_micro") == sess.get("qty", 0) * 10000,
                  "qty=%s amount=%s" % (sess.get("qty"), sess.get("amount_micro")))
        byt = items.get("bytes", {})
        q = byt.get("qty", 0)
        expect_bytes = q // 1048576 * 500000 + (q % 1048576) * 500000 // 1048576
        e2e.check("billing: 流量金额按单位取整",
                  byt.get("qty", 0) >= 8192 and byt.get("amount_micro") == expect_bytes,
                  "qty=%s amount=%s expect=%s" % (byt.get("qty"), byt.get("amount_micro"), expect_bytes))
        if tenants:
            total = sum(it.get("amount_micro", 0) for it in tenants[0].get("items", []))
            e2e.check("billing: 租户合计与明细一致", tenants[0].get("total_micro") == total,
                      "%s vs %s" % (tenants[0].get("total_micro"), total))

        # 接入令牌只能看本租户：租户 2 没有用量，汇总为空
        t2 = call("POST", "/api/v1/tenants", {"name": "计费租户二"})
        acc2 = call("POST", "/api/v1/client-accounts",
                    {"username": "billt2", "tenant_id": t2["id"]})
        call("POST", "/api/v1/permission/groups", {
            "name": "bill-t2", "description": "billing",
            "resources": ["billing.read", "billing.write"],
            "all_devices": True, "devices": [], "ports": "*",
            "members": [{"type": "client", "id": str(acc2["id"])}],
        })
        tok2 = call("POST", "/api/v1/client-accounts/%d/tokens" % acc2["id"], {"name": "bill"})["token"]
        req = urllib.request.Request(base + "/api/v1/billing/summary?from=%s&to=%s" % (day, day),
                                     headers={"Authorization": "Bearer " + tok2})
        import json as _json
        with urllib.request.urlopen(req, timeout=5) as r:
            t2sum = _json.loads(r.read().decode() or "{}")
        tids = [x.get("tenant_id") for x in t2sum.get("tenants", [])]
        e2e.check("billing: 接入令牌只看本租户", all(t == t2["id"] for t in tids), str(tids))

        # 价目是全局的：接入令牌即使被授了 billing.write 也不能改
        preq = urllib.request.Request(
            base + "/api/v1/billing/prices/bytes",
            data=_json.dumps({"unit_size": 1, "price_yuan": 9}).encode(), method="PUT")
        preq.add_header("Content-Type", "application/json")
        preq.add_header("Authorization", "Bearer " + tok2)
        code = 0
        try:
            with urllib.request.urlopen(preq, timeout=5) as r:
                code = r.status
        except urllib.error.HTTPError as e:
            code = e.code
        e2e.check("billing: 接入令牌不能改全局价目", code == 403, "status=%d" % code)
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
