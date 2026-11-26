#!/usr/bin/env python3
"""rt-mcp：把 rtunnel 管理 API 暴露成 MCP 工具（stdio）。

设计要点
- schema 不手写：从 relay 的 /api/v1/openapi.json 读（该文件由 api-docs.js 生成，
  而 api-docs.js 由 check:docs 强制与 admin_routes.cpp 对表）。接口变了工具自动跟上。
- 默认只读：只有 GET 变成工具；写操作要 --allow-write 显式打开。
- 令牌只从文件/环境读：不接受命令行明文（会进 shell 历史）。
- 授权仍由 relay 的权限组判定：这里不绕过、不缓存、不放大。

用法
  rt-mcp.py --rt https://relay:9443 --atk-file /etc/rtunnel/atk --insecure
  rt-mcp.py ... --allow-write --allow-prefix /api/v1/operations
  rt-mcp.py ... --list-tools          # 打印工具清单（调试）
  rt-mcp.py ... --print-openapi-url   # 只打印实际使用的 schema 来源

MCP 客户端配置示例（Claude Desktop 等）
  {
    "mcpServers": {
      "rtunnel": {
        "command": "python3",
        "args": ["/opt/rtunnel/deploy/rt-mcp.py",
                 "--rt", "https://relay:9443",
                 "--atk-file", "/etc/rtunnel/atk", "--insecure"]
      }
    }
  }
"""
import argparse
import json
import os
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "rtunnel"
SERVER_VERSION = "1.0"
MAX_TEXT = 200_000          # 单次工具结果上限：超过就截断，避免把模型上下文撑爆


def log(*a):
    """stdout 是 MCP 协议通道，日志一律走 stderr。"""
    print(*a, file=sys.stderr, flush=True)


class Client:
    def __init__(self, rt, atk, insecure=False, timeout=30):
        self.rt = rt.rstrip("/")
        self.atk = atk
        self.timeout = timeout
        self.ctx = None
        if self.rt.startswith("https://"):
            self.ctx = ssl.create_default_context()
            if insecure:
                self.ctx.check_hostname = False
                self.ctx.verify_mode = ssl.CERT_NONE
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=self.ctx))

    def fetch_json(self, url):
        """拉 OpenAPI：走同一个 TLS 配置并带上令牌（relay 上也可能要求认证）。"""
        req = urllib.request.Request(url)
        if self.atk:
            req.add_header("Authorization", "Bearer " + self.atk)
        with self.opener.open(req, timeout=self.timeout) as r:
            return json.loads(r.read().decode() or "{}")

    def request(self, method, path, query=None, body=None, headers=None):
        url = self.rt + path
        if query:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(query)
        data = None
        req = urllib.request.Request(url, method=method)
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode()
            req.data = data
            req.add_header("Content-Type", "application/json")
        if self.atk:
            req.add_header("Authorization", "Bearer " + self.atk)
        # 标记调用方，便于审计区分「人点的」和「模型点的」；只进审计，不参与授权
        req.add_header("X-Actor", os.environ.get("RT_ACTOR", "ai:rt-mcp"))
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with self.opener.open(req, timeout=self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except urllib.error.URLError as e:
            return 0, str(e.reason).encode()


def load_atk(args):
    if args.atk_file:
        with open(args.atk_file) as f:
            return f.read().strip()
    env = os.environ.get("ATK", "").strip()
    if not env:
        log("[rt-mcp] 警告：没有 --atk-file 也没有 ATK 环境变量，所有请求都会 401")
    return env


def fetch_openapi(client, url_or_path):
    if url_or_path.startswith("http://") or url_or_path.startswith("https://"):
        return client.fetch_json(url_or_path)
    with open(url_or_path) as f:
        return json.load(f)


# ---------------------------------------------------------------- 工具构造

def collect_tools(doc, allow_write, allow_prefix):
    tools = []
    for path, methods in sorted(doc.get("paths", {}).items()):
        for method, op in methods.items():
            read_only = op.get("x-rtunnel-read-only", method.lower() == "get")
            if not read_only and not allow_write:
                continue
            if allow_prefix and not any(path.startswith(p) for p in allow_prefix):
                continue
            tools.append(make_tool(path, method, op))
    return tools


def make_tool(path, method, op):
    props, required = {}, []

    for p in op.get("parameters", []):
        if p.get("in") not in ("path", "query", "header"):
            continue
        schema = dict(p.get("schema") or {"type": "string"})
        if p.get("description"):
            schema["description"] = p["description"]
        props[p["name"]] = schema
        if p.get("required"):
            required.append(p["name"])

    body_schema = None
    rb = op.get("requestBody") or {}
    for ct, media in (rb.get("content") or {}).items():
        if "application/json" in ct:
            body_schema = media.get("schema") or {"type": "object"}
            example = media.get("example")
            break
    if body_schema is not None:
        if not body_schema.get("properties"):
            body_schema = {"type": "object", "additionalProperties": True,
                           "description": "请求体（见工具描述里的示例）"}
        props["body"] = body_schema
        if rb.get("required"):
            required.append("body")

    desc = (op.get("summary") or path) + "\n\n" + method.upper() + " " + path
    if op.get("description"):
        desc += "\n\n" + op["description"]
    if not op.get("x-rtunnel-read-only", method.lower() == "get"):
        desc += "\n\n【写操作】会真实改变设备/系统状态；能走 operations 的先用 plan 预览。"

    return {
        "name": op.get("operationId") or (method + path),
        "description": desc.strip(),
        "inputSchema": {
            "type": "object",
            "properties": props,
            **({"required": required} if required else {}),
        },
        "_method": method.upper(),
        "_path": path,
    }


def do_call(tool, args):
    path = tool["_path"]
    query, headers, body = {}, {}, None
    props = tool["inputSchema"]["properties"]

    for name, schema in props.items():
        if name not in args or args[name] is None:
            continue
        value = args[name]
        if path and ("{" + name + "}") in path:
            path = path.replace("{" + name + "}", urllib.parse.quote(str(value), safe=""))
            continue
        if name == "body" and isinstance(value, dict):
            body = value
            continue
        # 未在 openapi 里声明 in 的按 query 处理
        query[name] = value

    return path, query, body, headers


# ---------------------------------------------------------------- JSON-RPC

def send(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reply(mid, result):
    send({"jsonrpc": "2.0", "id": mid, "result": result})


def reply_err(mid, code, message):
    send({"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}})


def run(args):
    atk = load_atk(args)
    client = Client(args.rt, atk, args.insecure, args.timeout)

    source = args.openapi or (client.rt + "/api/v1/openapi.json")
    try:
        doc = fetch_openapi(client, source)
    except Exception as e:      # noqa: BLE001 - 起不来就明确报错退出
        log("[rt-mcp] 读不到 OpenAPI（%s）：%s" % (source, e))
        log("[rt-mcp] 可用 --openapi 指定本地文件；或确认 relay 版本带 /api/v1/openapi.json")
        return 2

    tools = collect_tools(doc, args.allow_write, args.allow_prefix)
    by_name = {t["name"]: t for t in tools}
    log("[rt-mcp] schema=%s 工具=%d（只读=%d，--allow-write=%s）"
        % (source, len(tools), sum(1 for t in tools if t["_method"] == "GET"),
           "on" if args.allow_write else "off"))

    if args.list_tools:
        for t in tools:
            print("%-44s %s" % (t["name"], t["description"].splitlines()[0]))
        return 0

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            reply_err(None, -32700, "parse error")
            continue
        mid = msg.get("id")
        method = msg.get("method")
        params = msg.get("params") or {}

        if method == "initialize":
            reply(mid, {
                "protocolVersion": params.get("protocolVersion") or PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            })
        elif method in ("notifications/initialized", "initialized"):
            continue                      # 通知没有 id，不回
        elif method == "ping":
            reply(mid, {})
        elif method == "tools/list":
            reply(mid, {"tools": [{k: v for k, v in t.items() if not k.startswith("_")}
                                  for t in tools]})
        elif method == "tools/call":
            name = params.get("name")
            targs = params.get("arguments") or {}
            tool = by_name.get(name)
            if tool is None:
                reply_err(mid, -32602, "unknown tool: %s" % name)
                continue
            path, query, body, headers = do_call(tool, targs)
            status, raw = client.request(tool["_method"], path, query, body, headers)
            text = raw.decode("utf-8", "replace")
            if len(text) > MAX_TEXT:
                text = text[:MAX_TEXT] + "\n…（已截断，原长 %d 字节）" % len(raw)
            if status == 0:
                reply(mid, {"content": [{"type": "text", "text": "请求失败：" + text}],
                            "isError": True})
            elif status >= 400:
                reply(mid, {"content": [{"type": "text",
                                         "text": "HTTP %d\n%s" % (status, text)}],
                            "isError": True})
            else:
                reply(mid, {"content": [{"type": "text", "text": text}]})
        else:
            reply_err(mid, -32601, "method not found: %s" % method)
    return 0


def main():
    ap = argparse.ArgumentParser(description="rtunnel MCP server（stdio）")
    ap.add_argument("--rt", default=os.environ.get("RT", "http://127.0.0.1:9443"),
                    help="relay 管理面地址")
    ap.add_argument("--atk-file", help="atk- 令牌文件（0600）；也可用 ATK 环境变量")
    ap.add_argument("--insecure", action="store_true", help="跳过 TLS 证书校验")
    ap.add_argument("--openapi", help="OpenAPI 文件或 URL；默认 $RT/api/v1/openapi.json")
    ap.add_argument("--allow-write", action="store_true", help="同时暴露写操作（默认只读）")
    ap.add_argument("--allow-prefix", action="append", default=[],
                    help="只暴露该路径前缀下的工具，可重复")
    ap.add_argument("--timeout", type=int, default=30, help="单次请求超时（秒）")
    ap.add_argument("--list-tools", action="store_true", help="打印工具清单后退出")
    args = ap.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
