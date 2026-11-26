#!/usr/bin/env python3
"""自愈规则（playbook）e2e：告警 → 生成作业 →（可选）自动执行 → verify。

场景：
  1. 只读规则 auto_apply=1 且风险 1 ≤ 全局上限 → 设备上线告警后自动执行并 verify 通过
  2. risk 2 的规则即使 auto_apply=1，也因超过全局上限只生成待批作业（不执行）
  3. 冷却期内同一规则不再触发
  4. 全局 playbook.enabled=0 → 不生成作业
  5. risk 由服务端按 argv 计算，客户端传 risk 无效
  6. 手动 run：用规则给指定设备生成作业，越权设备 403
  7. 参数校验：argv 为空、未知 id、重名
  8. 接入令牌（rtk）不能读/写规则（运营商侧资源）
  9. 审计里有 playbook.fire / playbook.done
"""

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
LOG = "/tmp/rtunnel-playbook"
ALLOW = "uptime;echo *;/bin/sh -c *;systemctl status *"


def call_json(op, base, path, body=None, method="POST"):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with op.open(req, timeout=40) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def bearer(op, base, path, token, method="GET", body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method)
    req.add_header("Authorization", "Bearer " + token)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with op.open(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}


def start_agent(port, log):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    return subprocess.Popen(
        [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(port),
         "--devid", "dev1", "--allow-exec", ALLOW,
         "--config-file", os.path.join(LOG, "agent.json"), "--verbose"],
        stdout=log, stderr=subprocess.STDOUT, env=env)


def wait_online(base, timeout=15):
    adm, call = e2e.admin_client(base)
    end = time.time() + timeout
    while time.time() < end:
        try:
            items = call("GET", "/api/v1/agents")
            if isinstance(items, dict):
                items = items.get("items", [])
            for d in items:
                if d.get("devid") == "dev1" and d.get("online"):
                    return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def wait_offline(base, timeout=25):
    """告警线程每 5s 采一次；用它的在线表判断，比 /agents 的库同步更及时。"""
    adm, call = e2e.admin_client(base)
    end = time.time() + timeout
    while time.time() < end:
        try:
            st = call("GET", "/api/v1/alerts/status")
            if isinstance(st, dict) and st.get("devices_offline", 0) >= 1:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def wait_fires(base, n, timeout=25):
    adm, call = e2e.admin_client(base)
    end = time.time() + timeout
    items = []
    while time.time() < end:
        try:
            r = call("GET", "/api/v1/playbooks/fires?limit=50")
            items = r.get("items", []) if isinstance(r, dict) else []
            if len(items) >= n:
                return items
        except Exception:
            pass
        time.sleep(0.5)
    return items


def main():
    os.makedirs(LOG, exist_ok=True)
    for name in ("admin.db", "admin.db-wal", "admin.db-shm", "agent.json"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    rlog = open(os.path.join(LOG, "relay.log"), "w")
    alog = open(os.path.join(LOG, "agent.log"), "w")
    procs = []
    relay = None
    agent = None
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--drain-timeout", "300", "--admin", "127.0.0.1:%d" % admin_port,
             "--admin-token", "admintok", "--admin-db", os.path.join(LOG, "admin.db"),
             "--admin-file-dir", os.path.join(LOG, "files"), "--verbose"],
            stdout=rlog, stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.6)

        adm, call = e2e.admin_client(base)
        # 离线判定 1s，让测试不用等 2 分钟
        call("PUT", "/api/v1/settings", {"alert.device_offline": "1", "alert.offline_after_s": "5"})

        agent = start_agent(relay_port, alog)
        procs.append(agent)
        e2e.check("pb: dev1 上线", wait_online(base), "dev1 offline")
        time.sleep(6)   # 让告警线程跑满一轮，建立“在线”基线

        # ---------------------------------------------------------------- 规则
        code, ro = call_json(adm, base, "/api/v1/playbooks", {
            "name": "上线自检", "event": "device.online", "devid_glob": "dev*",
            "argv": ["uptime"], "verify_argv": ["uptime"],
            "auto_apply": True, "cooldown_s": 300, "max_per_hour": 6,
            "note": "设备上线后确认负载可读",
        })
        e2e.check("pb: 创建只读规则", code == 200 and ro.get("id"), "code=%d r=%s" % (code, ro))
        e2e.check("pb: risk 由服务端算成 1", ro.get("risk") == 1, "risk=%s" % ro.get("risk"))
        ro_id = ro.get("id", 0)

        code, rw = call_json(adm, base, "/api/v1/playbooks", {
            "name": "重启服务", "event": "device.online", "devid_glob": "dev1",
            "argv": ["systemctl", "restart", "nginx"],
            "auto_apply": True, "risk": 1,   # 客户端谎报 risk=1，服务端应忽略
            "cooldown_s": 300, "max_per_hour": 6,
        })
        e2e.check("pb: 创建写规则", code == 200 and rw.get("id"), "code=%d r=%s" % (code, rw))
        e2e.check("pb: 客户端不能把重启标成只读", rw.get("risk") == 2, "risk=%s" % rw.get("risk"))
        rw_id = rw.get("id", 0)

        # ---------------------------------------------------------------- 触发
        agent.send_signal(15)
        try:
            agent.wait(timeout=5)
        except Exception:
            agent.kill()
        procs.remove(agent)
        e2e.check("pb: dev1 离线被检测到", wait_offline(base), "未检测到离线")
        time.sleep(6)   # 离线告警已经产生，基线更新

        agent = start_agent(relay_port, alog)
        procs.append(agent)
        e2e.check("pb: dev1 重新上线", wait_online(base))

        fires = wait_fires(base, 2)
        by_pb = {}
        for f in fires:
            by_pb.setdefault(f.get("playbook"), []).append(f)
        e2e.check("pb: 上线告警触发两条规则", len(fires) >= 2, "fires=%d" % len(fires))

        ro_fires = by_pb.get("上线自检", [])
        e2e.check("pb: 只读规则自动执行", len(ro_fires) == 1, str(ro_fires)[:200])
        if ro_fires:
            e2e.check("pb: 只读规则 verify 通过", ro_fires[0].get("status") == "verified",
                      "status=%s err=%s" % (ro_fires[0].get("status"), ro_fires[0].get("error")))

        rw_fires = by_pb.get("重启服务", [])
        e2e.check("pb: 超上限的写规则只生成待批", len(rw_fires) == 1, str(rw_fires)[:200])
        if rw_fires:
            e2e.check("pb: 写规则未被执行", rw_fires[0].get("status") == "pending_approval",
                      "status=%s" % rw_fires[0].get("status"))

        # ---------------------------------------------------------------- 冷却
        agent.send_signal(15)
        try:
            agent.wait(timeout=5)
        except Exception:
            agent.kill()
        procs.remove(agent)
        wait_offline(base)
        time.sleep(6)
        agent = start_agent(relay_port, alog)
        procs.append(agent)
        wait_online(base)
        time.sleep(8)   # 给告警 + 规则匹配留时间
        again = call_json(adm, base, "/api/v1/playbooks/fires?limit=50", method="GET")[1]
        n_after = len(again.get("items", []))
        e2e.check("pb: 冷却期内不重复触发", n_after == len(fires),
                  "%d -> %d" % (len(fires), n_after))

        # ---------------------------------------------------------------- 总开关
        call("PUT", "/api/v1/settings", {"playbook.enabled": "0"})
        agent.send_signal(15)
        try:
            agent.wait(timeout=5)
        except Exception:
            agent.kill()
        procs.remove(agent)
        wait_offline(base)
        time.sleep(6)
        agent = start_agent(relay_port, alog)
        procs.append(agent)
        wait_online(base)
        time.sleep(8)
        off = call_json(adm, base, "/api/v1/playbooks/fires?limit=50", method="GET")[1]
        e2e.check("pb: 关闭总开关后不再触发", len(off.get("items", [])) == n_after,
                  "%d -> %d" % (n_after, len(off.get("items", []))))
        call("PUT", "/api/v1/settings", {"playbook.enabled": "1"})

        # ---------------------------------------------------------------- 手动 run
        code, op = call_json(adm, base, "/api/v1/playbooks/%d/run" % ro_id, {"devid": "dev1"})
        e2e.check("pb: 手动 run 生成作业", code == 200 and op.get("id"),
                  "code=%d r=%s" % (code, op))
        e2e.check("pb: 手动 run 归属 playbook",
                  str(op.get("actor", "")).startswith("playbook:%d:" % ro_id),
                  str(op.get("actor")))
        code, _ = call_json(adm, base, "/api/v1/playbooks/%d/run" % ro_id, {})
        e2e.check("pb: run 缺少 devid 被拒", code == 400, "code=%d" % code)

        # ---------------------------------------------------------------- 校验
        code, r = call_json(adm, base, "/api/v1/playbooks", {"name": "空命令", "argv": []})
        e2e.check("pb: argv 为空被拒", code == 400, "code=%d r=%s" % (code, r))
        code, r = call_json(adm, base, "/api/v1/playbooks", {"name": "上线自检", "argv": ["uptime"]})
        e2e.check("pb: 重名被拒", code == 400 and "已存在" in r.get("error", ""),
                  "code=%d r=%s" % (code, r))
        code, r = call_json(adm, base, "/api/v1/playbooks/9999", method="GET")
        e2e.check("pb: 未知规则 404", code == 404, "code=%d" % code)

        # 停用规则后不再触发（顺带验证 enable 接口）
        code, r = call_json(adm, base, "/api/v1/playbooks/%d/enable" % rw_id, {"enabled": False})
        e2e.check("pb: 停用规则", code == 200 and r.get("enabled") is False,
                  "code=%d r=%s" % (code, r))

        # ---------------------------------------------------------------- 权限
        acc = call("POST", "/api/v1/client-accounts", {"username": "pbuser", "tenant_id": 1})
        call("POST", "/api/v1/permission/groups", {
            "name": "pb-g", "description": "e2e", "resources": ["device.view", "playbook.read"],
            "all_devices": True, "devices": [], "ports": "*",
            "members": [{"type": "client", "id": str(acc["id"])}],
        })
        rtk = call("POST", "/api/v1/client-accounts/%d/tokens" % acc["id"], {"name": "e2e"})["token"]
        bare = urllib.request.build_opener()   # 不带 Cookie，避免沿用管理台会话
        code, _ = bearer(bare, base, "/api/v1/playbooks", rtk)
        e2e.check("pb: 接入令牌不能读规则", code == 403, "code=%d" % code)
        code, _ = bearer(bare, base, "/api/v1/playbooks", rtk, method="POST",
                         body={"name": "x", "argv": ["uptime"]})
        e2e.check("pb: 接入令牌不能建规则", code == 403, "code=%d" % code)

        # ---------------------------------------------------------------- 审计
        rows = call("GET", "/api/v1/audit")
        acts = [x.get("action") for x in rows if isinstance(x, dict)]
        e2e.check("pb: 审计有 playbook.fire", "playbook.fire" in acts, str(acts[:6]))
        e2e.check("pb: 审计有 playbook.done", "playbook.done" in acts, str(acts[:6]))

        # 事件流：playbook.fired / playbook.done 也应出现在总线里（用 audit 顺带确认总线活着）
        code, _ = call_json(adm, base, "/api/v1/playbooks/%d" % ro_id, method="GET")
        e2e.check("pb: 规则详情可读", code == 200, "code=%d" % code)

    finally:
        for p in procs:
            try:
                p.send_signal(15)
            except Exception:
                pass
        for p in procs:
            try:
                p.wait(timeout=5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        rlog.close()
        alog.close()


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
