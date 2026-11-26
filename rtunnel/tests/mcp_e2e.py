#!/usr/bin/env python3
"""OpenAPI 与 MCP server 端到端：schema 来自 api-docs.js，工具由 schema 生成。

验证的四件事：
1. relay 能提供 /api/v1/openapi.json（磁盘 docroot 与内嵌两种来源走同一个 serveFrontend）
2. 工具清单与 OpenAPI 一致，默认只暴露只读工具
3. tools/call 真的打到管理 API，且带上了 atk 与 X-Actor
4. 失败路径：未知工具、错令牌、写操作被默认挡住
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-mcp"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MCP = os.path.join(ROOT, "rtunnel", "deploy", "rt-mcp.py")
DIST = os.path.join(ROOT, "rtunnel", "relay", "admin", "web", "dist")


class Mcp:
    """最小 MCP 客户端：stdio + 换行分隔的 JSON-RPC。"""

    def __init__(self, args):
        self.p = subprocess.Popen([sys.executable, MCP] + args,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.seq = 0

    def call(self, method, params=None, timeout=20):
        self.seq += 1
        msg = {"jsonrpc": "2.0", "id": self.seq, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        # 只读到自己的 id：通知/日志不会插在这里，但仍按 id 匹配更稳
        end = time.time() + timeout
        while time.time() < end:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("mcp 进程退出：%s" % self.p.stderr.read()[:400])
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if r.get("id") == self.seq:
                return r
        raise RuntimeError("mcp 响应超时")

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.p.kill()


def tool_text(r):
    res = r.get("result") or {}
    items = res.get("content") or []
    return (items[0].get("text") if items else ""), bool(res.get("isError"))


def main():
    os.makedirs(LOG, exist_ok=True)
    for name in ("admin.db", "admin.db-wal", "admin.db-shm"):
        try:
            os.remove(os.path.join(LOG, name))
        except OSError:
            pass

    relay_port, admin_port = e2e.free_port(), e2e.free_port()
    base = "http://127.0.0.1:%d" % admin_port
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")

    logs = {n: open(os.path.join(LOG, n + ".log"), "w") for n in ("relay",)}
    procs = []
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "admintok",
             "--admin-db", os.path.join(LOG, "admin.db"),
             "--admin-file-dir", os.path.join(LOG, "files"),
             "--admin-docroot", DIST, "--verbose"],
            stdout=logs["relay"], stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.8)

        admin_op, call = e2e.admin_client(base)
        atk = call("POST", "/api/v1/admin-tokens", {"name": "mcp-test"})["token"]
        atk_file = os.path.join(LOG, "atk")
        with open(atk_file, "w") as f:
            f.write(atk)
        os.chmod(atk_file, 0o600)

        # 1) OpenAPI 可用，且操作数与文档一致
        with admin_op.open(base + "/api/v1/openapi.json", timeout=10) as r:
            doc = json.loads(r.read().decode())
        ops = [(m.upper(), p, o) for p, ms in doc["paths"].items() for m, o in ms.items()]
        ro = [x for x in ops if x[2].get("x-rtunnel-read-only")]
        e2e.check("openapi: 可获取", doc.get("openapi", "").startswith("3."), doc.get("openapi"))
        e2e.check("openapi: 操作数与文档一致", len(ops) >= 130, "ops=%d" % len(ops))
        e2e.check("openapi: 标记只读操作", len(ro) >= 50, "readonly=%d" % len(ro))
        ids = [x[2]["operationId"] for x in ops]
        e2e.check("openapi: operationId 唯一", len(ids) == len(set(ids)),
                  "%d/%d" % (len(set(ids)), len(ids)))

        # 2) 默认只读：写操作不出现在工具里
        mcp = Mcp(["--rt", base, "--atk-file", atk_file])
        init = mcp.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                       "clientInfo": {"name": "e2e", "version": "0"}})
        mcp.notify("notifications/initialized")
        e2e.check("mcp: initialize 返回 serverInfo",
                  (init.get("result") or {}).get("serverInfo", {}).get("name") == "rtunnel",
                  str(init)[:160])

        tools = mcp.call("tools/list")["result"]["tools"]
        names = [t["name"] for t in tools]
        e2e.check("mcp: 只读工具数与 openapi 一致", len(tools) == len(ro),
                  "tools=%d readonly=%d" % (len(tools), len(ro)))
        e2e.check("mcp: 含设备列表工具", "get_agents" in names, str(names[:6]))
        e2e.check("mcp: 默认不含写工具", "post_agents_devid_exec" not in names, "")
        e2e.check("mcp: 工具带 inputSchema",
                  all(isinstance(t.get("inputSchema"), dict) for t in tools), "")
        devid_tool = next(t for t in tools if t["name"] == "get_agents_devid")
        e2e.check("mcp: 路径参数进入 schema",
                  "devid" in devid_tool["inputSchema"]["properties"]
                  and "devid" in (devid_tool["inputSchema"].get("required") or []),
                  json.dumps(devid_tool["inputSchema"])[:200])

        # 3) 真调：设备列表；设备不存在时仍应是 JSON 错误体而不是工具崩溃
        txt, is_err = tool_text(mcp.call("tools/call", {"name": "get_agents", "arguments": {}}))
        e2e.check("mcp: tools/call 调用成功", not is_err, txt[:160])
        e2e.check("mcp: 返回 JSON", txt.strip().startswith(("[", "{")), txt[:80])

        txt, is_err = tool_text(mcp.call("tools/call",
                                        {"name": "get_agents_devid",
                                         "arguments": {"devid": "nobody"}}))
        e2e.check("mcp: 设备不存在时 isError", is_err and "404" in txt, txt[:120])

        bad = mcp.call("tools/call", {"name": "no_such_tool", "arguments": {}})
        e2e.check("mcp: 未知工具返回 JSON-RPC 错误",
                  (bad.get("error") or {}).get("code") == -32602, str(bad)[:120])
        mcp.close()

        # 4) 写工具要显式打开，且带前缀过滤
        mcp = Mcp(["--rt", base, "--atk-file", atk_file, "--allow-write"])
        mcp.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
        mcp.notify("notifications/initialized")
        wtools = mcp.call("tools/list")["result"]["tools"]
        wnames = [t["name"] for t in wtools]
        e2e.check("mcp: --allow-write 暴露写工具", "post_agents_devid_exec" in wnames, "")
        e2e.check("mcp: 写工具数量多于只读", len(wnames) > len(names),
                  "%d vs %d" % (len(wnames), len(names)))
        put = next((t for t in wtools if t["name"] == "put_settings"), None)
        e2e.check("mcp: 写工具带 body 参数",
                  put is not None and "body" in put["inputSchema"]["properties"],
                  json.dumps(put["inputSchema"] if put else {})[:200])

        # 真改一次设置：这是写操作，会进审计
        txt, is_err = tool_text(mcp.call("tools/call", {
            "name": "put_settings", "arguments": {"body": {"alert.cooldown_s": "305"}}}))
        e2e.check("mcp: 写工具调用成功", not is_err, txt[:160])
        mcp.close()

        mcp = Mcp(["--rt", base, "--atk-file", atk_file, "--allow-write",
                   "--allow-prefix", "/api/v1/operations"])
        mcp.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
        mcp.notify("notifications/initialized")
        pnames = [t["name"] for t in mcp.call("tools/list")["result"]["tools"]]
        e2e.check("mcp: 前缀过滤生效",
                  pnames and all("operations" in n or n in ("get_operations",) for n in pnames),
                  str(pnames[:6]))
        mcp.close()

        # 5) 错令牌：工具照常列出，调用返回 isError（不能假装成功）
        bad_file = os.path.join(LOG, "atk-bad")
        with open(bad_file, "w") as f:
            f.write("atk-bad")
        mcp = Mcp(["--rt", base, "--atk-file", bad_file])
        mcp.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
        mcp.notify("notifications/initialized")
        txt, is_err = tool_text(mcp.call("tools/call", {"name": "get_agents", "arguments": {}}))
        e2e.check("mcp: 无效令牌返回 isError", is_err and "401" in txt, txt[:120])
        mcp.close()

        # 6) 审计里能看到模型来源（写操作才写审计，所以看刚改的设置）
        rows = call("GET", "/api/v1/audit")
        acts = [x.get("actor", "") if isinstance(x, dict) else str(x) for x in rows]
        e2e.check("mcp: 调用进入审计且带 ai 标记",
                  any("ai:rt-mcp" in a for a in acts), str(acts[:4]))
        e2e.check("mcp: 审计仍带真实账号",
                  any("ai:rt-mcp|" in a and "#" in a for a in acts), str(acts[:4]))
    finally:
        for p in procs:
            try:
                p.send_signal(15)
                p.wait(timeout=5)
            except Exception:  # noqa: BLE001
                p.kill()
        for f in logs.values():
            f.close()


if __name__ == "__main__":
    main()
    sys.exit(e2e.finish())
