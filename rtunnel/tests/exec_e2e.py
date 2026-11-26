#!/usr/bin/env python3
"""受控命令执行 e2e：白名单、stdout/stderr 分离、退出码、超时、截断与权限。

场景：
  1. 白名单外命令被拒（不允许 rm）
  2. echo 命中白名单 → stdout 正确、exit_code=0
  3. sh -c 脚本 → stdout/stderr 分流、非零退出码
  4. sleep 超出 timeout_ms → timed_out=true，耗时接近上限
  5. 大量输出被截断 → truncated=true、字节数不超过 max_bytes
  6. 未启用 --allow-exec 的设备 → 501
  7. 没有 device.exec 的接入令牌 → 403
  8. shell=true / argv 为空 / 非法 json → 400
  9. 审计里有 device.exec 记录
"""

import http.cookiejar
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-exec"
ALLOW = "echo *;/bin/sh -c *;sleep *;cat *"


def post_exec(base, devid, body=None, adm_op=None):
    data = json.dumps(body).encode()
    req = urllib.request.Request(base + "/api/v1/agents/%s/exec" % devid, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        if adm_op is None:
            with urllib.request.urlopen(req, timeout=40) as r:
                return r.status, json.loads(r.read().decode() or "{}")
        with adm_op.open(req, timeout=40) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def bearer_post(base, path, token, body):
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def post_json(op, base, path, body, method="POST", headers=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with op.open(req, timeout=40) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def login(base, user, password):
    """另起一个管理台会话（用于“另一个账号审批”）。"""
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    req = urllib.request.Request(base + "/api/v1/login",
                                 data=json.dumps({"username": user, "password": password}).encode(),
                                 method="POST")
    req.add_header("Content-Type", "application/json")
    op.open(req, timeout=5).read()
    return op


def wait_agent(base, devid, timeout=15):
    adm_op, call = e2e.admin_client(base)
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
    for name in ("admin.db", "admin.db-wal", "admin.db-shm"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass
    for name in ("agent1.json", "agent2.json"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    logs = {k: open(os.path.join(LOG, k + ".log"), "w") for k in ("relay", "agent1", "agent2")}
    procs = []
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--drain-timeout", "300", "--admin", "127.0.0.1:%d" % admin_port,
             "--admin-token", "admintok", "--admin-db", os.path.join(LOG, "admin.db"),
             "--admin-file-dir", os.path.join(LOG, "files"), "--verbose"],
            stdout=logs["relay"], stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.6)

        agent1 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow-exec", ALLOW,
             "--config-file", os.path.join(LOG, "agent1.json"), "--verbose"],
            stdout=logs["agent1"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent1)
        agent2 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev2", "--config-file", os.path.join(LOG, "agent2.json"), "--verbose"],
            stdout=logs["agent2"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent2)

        adm_op, _call = e2e.admin_client(base)
        e2e.check("exec: dev1 上线", wait_agent(base, "dev1"), "dev1 offline")
        e2e.check("exec: dev2 上线", wait_agent(base, "dev2"), "dev2 offline")

        # 1) 白名单外命令
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["rm", "-rf", "/tmp/nope"]})
        e2e.check("exec: 白名单外命令被拒", code == 200 and not r.get("ok") and "白名单" in r.get("error", ""),
                  "code=%d r=%s" % (code, r))

        # 2) 正常执行
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["echo", "hello"]})
        e2e.check("exec: echo 返回 stdout", code == 200 and r.get("ok") and r.get("stdout") == "hello\n",
                  "code=%d r=%s" % (code, r))
        e2e.check("exec: exit_code=0", r.get("exit_code") == 0, "exit=%s" % r.get("exit_code"))

        # 3) stdout/stderr 分流 + 非零退出
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["/bin/sh", "-c", "echo out; echo err >&2; exit 3"]})
        e2e.check("exec: stdout/stderr 分流",
                  code == 200 and r.get("stdout", "").strip() == "out" and r.get("stderr", "").strip() == "err",
                  "code=%d r=%s" % (code, r))
        e2e.check("exec: 非零退出码", r.get("exit_code") == 3, "exit=%s" % r.get("exit_code"))

        # 4) 超时
        t0 = time.time()
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["sleep", "5"], "timeout_ms": 1000})
        elapsed = time.time() - t0
        e2e.check("exec: 超时被终止", code == 200 and r.get("timed_out") is True,
                  "code=%d r=%s" % (code, r))
        e2e.check("exec: 超时耗时接近上限", 0.8 <= elapsed <= 4.0, "%.2fs" % elapsed)

        # 5) 输出截断
        code, r = post_exec(base, "dev1", adm_op=adm_op,
                            body={"argv": ["/bin/sh", "-c", "yes x | head -c 200000"], "max_bytes": 4096})
        e2e.check("exec: 输出被截断", code == 200 and r.get("truncated") is True,
                  "code=%d r=%s" % (code, r))
        e2e.check("exec: 截断后字节数不超上限",
                  r.get("stdout_bytes", 0) + r.get("stderr_bytes", 0) <= 4096,
                  "bytes=%s" % (r.get("stdout_bytes", 0) + r.get("stderr_bytes", 0)))

        # 6) 未启用 exec 的设备
        code, r = post_exec(base, "dev2", adm_op=adm_op, body={"argv": ["echo", "hi"]})
        e2e.check("exec: 未启用返回 501", code == 501, "code=%d r=%s" % (code, r))

        # 7) 权限：没有 device.exec 的接入令牌
        tok = e2e.create_client_token(base, "execuser", grant_all=True)
        code, r = bearer_post(base, "/api/v1/agents/dev1/exec", tok, {"argv": ["echo", "hi"]})
        e2e.check("exec: 无 device.exec 的令牌被拒", code == 403, "code=%d r=%s" % (code, r))

        # 8) 参数校验
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["echo", "x"], "shell": True})
        e2e.check("exec: 拒绝 shell 模式", code == 400, "code=%d" % code)
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": []})
        e2e.check("exec: 空 argv 拒绝", code == 400, "code=%d" % code)
        code, r = post_exec(base, "dev1", adm_op=adm_op, body={"argv": ["echo", "x"], "timeout_ms": 60000})
        e2e.check("exec: 超长 timeout 拒绝", code == 400, "code=%d" % code)

        # 8.5) 只读作业：risk 1 不需要审批，plan 后可直接 apply
        code, plan = post_json(adm_op, base, "/api/v1/operations/plan",
                               {"kind": "exec", "devid": "dev1", "argv": ["echo", "ro"]})
        e2e.check("ops: 只读作业 plan 成功", code == 200 and plan.get("id"), "code=%d" % code)
        ro_id = plan.get("id", 0)
        e2e.check("ops: 只读作业无需审批", plan.get("status") == "planned" and plan.get("risk") == 1,
                  "status=%s risk=%s" % (plan.get("status"), plan.get("risk")))
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % ro_id,
                            {"plan_hash": plan.get("plan_hash")})
        e2e.check("ops: 只读作业可直接 apply", code == 200 and r.get("status") == "applied",
                  "code=%d r=%s" % (code, r))

        # 8.6) 写作业：默认阈值 2，落库即待审批
        cnt = os.path.join(LOG, "opcnt")
        try:
            os.remove(cnt)
        except OSError:
            pass
        code, plan = post_json(adm_op, base, "/api/v1/operations/plan",
                               {"kind": "exec", "devid": "dev1",
                                "argv": ["/bin/sh", "-c", "echo x >> " + cnt]})
        op_id = plan.get("id", 0)
        e2e.check("ops: 写作业进入待审批", code == 200 and plan.get("status") == "pending_approval",
                  "code=%d status=%s" % (code, plan.get("status")))

        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % op_id,
                            {"plan_hash": plan.get("plan_hash")})
        e2e.check("ops: 待审批时 apply 被拒", code == 409, "code=%d r=%s" % (code, r))

        # 自己批自己必须拒绝
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/approve" % op_id,
                            {"decision": "approve"})
        e2e.check("ops: 不能批准自己发起的作业", code == 403, "code=%d r=%s" % (code, r))

        # 第二个账号（另一个超管）审批
        # 第二个账号：manager + 一个覆盖全部设备的组（含 op.approve）。
        # 内置 manager 组本身有 op.approve，但设备范围为空的账号管不到任何设备。
        _call("POST", "/api/v1/users", {"username": "approver", "password": "approver1", "role": "manager"})
        # 组成员里的 user 用数字 id（与 /permission/subjects 一致），不是用户名
        users = _call("GET", "/api/v1/users")
        uid = next((u["id"] for u in users if u.get("username") == "approver"), 0)
        # 成员必须用数字 id：传用户名会被拒，而不是静默写入一条永不匹配的成员
        code, r = post_json(adm_op, base, "/api/v1/permission/groups", {
            "name": "e2e-badmember", "description": "e2e", "resources": ["op.approve"],
            "all_devices": False, "devices": [], "ports": "*",
            "members": [{"type": "user", "id": "approver"}],
        })
        e2e.check("perm: 成员用用户名被拒", code == 400, "code=%d r=%s" % (code, r))
        _call("POST", "/api/v1/permission/groups", {
            "name": "e2e-approvers", "description": "e2e",
            "resources": ["device.view", "device.exec", "device.proc.read", "op.approve"],
            "all_devices": True, "devices": [], "ports": "*",
            "members": [{"type": "user", "id": str(uid)}],
        })
        op2 = login(base, "approver", "approver1")
        code, r = post_json(op2, base, "/api/v1/operations/%d/approve" % op_id,
                            {"decision": "approve", "note": "e2e ok"})
        e2e.check("ops: 另一账号可批准", code == 200 and r.get("status") == "approved",
                  "code=%d r=%s" % (code, r))
        e2e.check("ops: 记录审批人", r.get("approver") not in (None, "", plan.get("actor")),
                  "approver=%s actor=%s" % (r.get("approver"), plan.get("actor")))

        # 错的 plan_hash 仍然拒绝
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % op_id, {"plan_hash": "deadbeef"})
        e2e.check("ops: plan_hash 不匹配被拒", code == 409, "code=%d" % code)

        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % op_id,
                            {"plan_hash": plan.get("plan_hash")})
        e2e.check("ops: 批准后 apply 成功", code == 200 and r.get("status") == "applied",
                  "code=%d r=%s" % (code, r))

        # 重放：同一个 apply 再打一次，命令不应再执行
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % op_id,
                            {"plan_hash": plan.get("plan_hash")})
        e2e.check("ops: 重复 apply 幂等", code == 200 and r.get("status") == "applied",
                  "code=%d status=%s" % (code, r.get("status")))
        with open(cnt) as f:
            lines = [x for x in f.read().split("\n") if x]
        e2e.check("ops: 命令只执行一次", len(lines) == 1, "lines=%d" % len(lines))

        code, r = post_json(adm_op, base, "/api/v1/operations/%d/verify" % op_id, {})
        e2e.check("ops: verify 通过", code == 200 and r.get("verified") is True,
                  "code=%d r=%s" % (code, r))

        # 8.7) 拒绝：状态变 rejected，之后 apply 与再次审批都失败
        code, plan2 = post_json(adm_op, base, "/api/v1/operations/plan",
                                {"kind": "exec", "devid": "dev1",
                                 "argv": ["/bin/sh", "-c", "echo never >> " + cnt]})
        rid = plan2.get("id", 0)
        code, r = post_json(op2, base, "/api/v1/operations/%d/approve" % rid,
                            {"decision": "reject", "note": "不需要"})
        e2e.check("ops: 可以拒绝作业", code == 200 and r.get("status") == "rejected",
                  "code=%d r=%s" % (code, r))
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % rid,
                            {"plan_hash": plan2.get("plan_hash")})
        e2e.check("ops: 已拒绝的作业不能执行", code == 409, "code=%d" % code)
        code, r = post_json(op2, base, "/api/v1/operations/%d/approve" % rid, {"decision": "approve"})
        e2e.check("ops: 已拒绝的作业不能再批准", code == 409, "code=%d" % code)
        with open(cnt) as f:
            lines = [x for x in f.read().split("\n") if x]
        e2e.check("ops: 被拒绝的命令没有执行", len(lines) == 1, "lines=%d" % len(lines))

        # 8.8) 执行窗口：把窗口设成当前时间之后，apply 被拒
        now = time.localtime()
        start = (now.tm_hour * 60 + now.tm_min + 2) % 1440
        end = (start + 1) % 1440
        win = "%02d:%02d-%02d:%02d" % (start // 60, start % 60, end // 60, end % 60)
        _call("PUT", "/api/v1/settings", {"ops.window": win})
        code, planw = post_json(adm_op, base, "/api/v1/operations/plan",
                                {"kind": "exec", "devid": "dev1", "argv": ["echo", "win"]})
        code, r = post_json(adm_op, base, "/api/v1/operations/%d/apply" % planw.get("id", 0),
                            {"plan_hash": planw.get("plan_hash")})
        e2e.check("ops: 不在执行窗口时拒绝", code == 403, "code=%d win=%s r=%s" % (code, win, r))
        _call("PUT", "/api/v1/settings", {"ops.window": ""})

        # 8.9) 配额：每分钟 1 个
        _call("PUT", "/api/v1/settings", {"ops.max_per_minute": "1"})
        code, p1 = post_json(adm_op, base, "/api/v1/operations/plan",
                             {"kind": "exec", "devid": "dev1", "argv": ["echo", "q1"]})
        code, r1 = post_json(adm_op, base, "/api/v1/operations/%d/apply" % p1.get("id", 0),
                             {"plan_hash": p1.get("plan_hash")})
        code, p2b = post_json(adm_op, base, "/api/v1/operations/plan",
                              {"kind": "exec", "devid": "dev1", "argv": ["echo", "q2"]})
        code, r2 = post_json(adm_op, base, "/api/v1/operations/%d/apply" % p2b.get("id", 0),
                             {"plan_hash": p2b.get("plan_hash")})
        e2e.check("ops: 超出每分钟配额被拒", code == 429, "code=%d r=%s" % (code, r2))
        _call("PUT", "/api/v1/settings", {"ops.max_per_minute": "0"})

        # 8.10) 同 Idempotency-Key 的 plan 返回同一作业
        code, p2 = post_json(adm_op, base, "/api/v1/operations/plan",
                             {"kind": "exec", "devid": "dev1", "argv": ["echo", "idem"]},
                             headers={"Idempotency-Key": "k-" + str(op_id)})
        code, p3 = post_json(adm_op, base, "/api/v1/operations/plan",
                             {"kind": "exec", "devid": "dev1", "argv": ["echo", "idem"]},
                             headers={"Idempotency-Key": "k-" + str(op_id)})
        e2e.check("ops: 幂等键复用同一作业", p2.get("id") == p3.get("id") and p2.get("id") != 0,
                  "%s vs %s" % (p2.get("id"), p3.get("id")))

        code, lst = post_json(adm_op, base, "/api/v1/operations?devid=dev1&limit=30", None, method="GET")
        e2e.check("ops: 列表含刚执行的作业",
                  code == 200 and any(x.get("id") == op_id for x in lst.get("items", [])),
                  "code=%d items=%d" % (code, len(lst.get("items", []))))
        code, pend = post_json(adm_op, base, "/api/v1/operations?status=pending_approval", None,
                               method="GET")
        e2e.check("ops: 可按状态筛选待审批",
                  code == 200 and all(x.get("status") == "pending_approval" for x in pend.get("items", [])),
                  "code=%d items=%d" % (code, len(pend.get("items", []))))

        # 9) 审计
        rows = _call("GET", "/api/v1/audit")
        hit = [x for x in rows if x.get("action") == "device.exec" and "echo hello" in x.get("detail", "")]
        e2e.check("exec: 写入了审计", bool(hit), "rows=%d" % len(rows))
        if hit:
            e2e.check("exec: 审计含命令与退出码", "exit=0" in hit[0].get("detail", ""), str(hit[0]))
            e2e.check("exec: 审计主体不是 anonymous", hit[0].get("actor") != "anonymous", str(hit[0]))
    finally:
        for p in procs:
            if p.poll() is None:
                p.send_signal(15)
        for p in procs:
            try:
                p.wait(timeout=5)
            except Exception:
                p.kill()
        for f in logs.values():
            f.close()


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
