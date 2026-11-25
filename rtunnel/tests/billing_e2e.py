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
        admin_op, call = e2e.admin_client(base)

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

        # ---- 结算单：账期快照 + 状态机 ----
        period = time.strftime("%Y-%m")
        tid1 = tenants[0]["tenant_id"] if tenants else 1
        total1 = tenants[0]["total_micro"] if tenants else 0
        gen = call("POST", "/api/v1/billing/statements", {"period": period})
        e2e.check("stat: 生成结算单", len(gen.get("created", [])) >= 1,
                  str(gen)[:200])
        stmts = call("GET", "/api/v1/billing/statements?period=%s" % period).get("items", [])
        mine = [s for s in stmts if s.get("tenant_id") == tid1]
        e2e.check("stat: 列表可见且为草稿", len(mine) == 1 and mine[0].get("status") == "draft",
                  str(mine)[:200])
        sid = mine[0]["id"] if mine else 0
        e2e.check("stat: 金额等于试算合计", mine and mine[0].get("total_micro") == total1,
                  "stmt=%s total=%s" % (mine[0].get("total_micro") if mine else None, total1))
        e2e.check("stat: 明细已快照", mine and len(mine[0].get("lines", [])) >= 1,
                  str(mine[0].get("lines") if mine else None)[:160])

        # 草稿可重算，金额不重复累加
        gen2 = call("POST", "/api/v1/billing/statements", {"period": period})
        e2e.check("stat: 草稿重算走更新", len(gen2.get("updated", [])) >= 1, str(gen2)[:160])
        again = call("GET", "/api/v1/billing/statements/%d" % sid)
        e2e.check("stat: 重算不翻倍", again.get("total_micro") == total1,
                  "%s vs %s" % (again.get("total_micro"), total1))

        # 草稿不能直接标记已收款
        code = 0
        try:
            call("PUT", "/api/v1/billing/statements/%d" % sid, {"status": "paid"})
        except urllib.error.HTTPError as e:
            code = e.code
        e2e.check("stat: 草稿不能直接收款", code == 400, "status=%d" % code)

        issued = call("PUT", "/api/v1/billing/statements/%d" % sid,
                      {"status": "issued", "note": "2026-11 账期"})
        e2e.check("stat: 出账", issued.get("status") == "issued" and issued.get("issued_ms", 0) > 0,
                  str(issued)[:160])
        e2e.check("stat: 备注落库", issued.get("note") == "2026-11 账期", issued.get("note"))

        # 已出账不能再重算，也不会被新用量改写
        gen3 = call("POST", "/api/v1/billing/statements", {"period": period})
        sk = [x for x in gen3.get("skipped", []) if x.get("tenant_id") == tid1]
        e2e.check("stat: 已出账不重算", len(sk) == 1 and "已出账" in sk[0].get("reason", ""),
                  str(gen3.get("skipped"))[:160])

        # CSV：带 BOM、表头与明细行
        creq = urllib.request.Request(base + "/api/v1/billing/statements/%d/csv" % sid)
        with admin_op.open(creq, timeout=5) as r:
            csv_body = r.read().decode("utf-8-sig")
            ctype = r.headers.get("Content-Type", "")
        e2e.check("stat: CSV 可导出",
                  "text/csv" in ctype and "tenant_id,tenant_name,period" in csv_body and
                  period in csv_body, ctype + " / " + csv_body[:80])

        paid = call("PUT", "/api/v1/billing/statements/%d" % sid, {"status": "paid"})
        e2e.check("stat: 标记已收款", paid.get("status") == "paid" and paid.get("paid_ms", 0) > 0,
                  str(paid)[:160])

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

        # 结算单同样受租户与权限约束
        def bearer_status(path, token, method="GET", body=None):
            data = _json.dumps(body).encode() if body is not None else None
            rq = urllib.request.Request(base + path, data=data, method=method)
            if data is not None:
                rq.add_header("Content-Type", "application/json")
            rq.add_header("Authorization", "Bearer " + token)
            try:
                with urllib.request.urlopen(rq, timeout=5) as r:
                    return r.status, _json.loads(r.read().decode() or "{}")
            except urllib.error.HTTPError as e:
                return e.code, {}

        code, _ = bearer_status("/api/v1/billing/statements", tok2, "POST", {"period": period})
        e2e.check("stat: 接入令牌不能生成结算单", code == 403, "status=%d" % code)
        code, body = bearer_status("/api/v1/billing/statements?period=%s" % period, tok2)
        e2e.check("stat: 令牌只看本租户结算单",
                  code == 200 and all(s.get("tenant_id") == t2["id"] for s in body.get("items", [])),
                  "status=%d items=%s" % (code, body.get("items")))
        code, _ = bearer_status("/api/v1/billing/statements/%d" % sid, tok2)
        e2e.check("stat: 不能读别家结算单", code == 403, "status=%d" % code)
        code, _ = bearer_status("/api/v1/billing/statements/%d/csv" % sid, tok2)
        e2e.check("stat: 不能导出别家结算单", code == 403, "status=%d" % code)
        code, _ = bearer_status("/api/v1/billing/statements/%d" % sid, tok2, "PUT", {"status": "void"})
        e2e.check("stat: 不能改别家结算单状态", code == 403, "status=%d" % code)
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
