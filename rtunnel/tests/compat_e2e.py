#!/usr/bin/env python3
"""RTNP 握手 / 版本兼容回归。

验证：
  1. 语义版本等于 relay 当前版本时，HELLO_ACK 尾部带回 (proto, proto_min)
  2. 低于 PROTO_MIN_VERSION 的 HELLO 被 AUTH_FAIL 拒绝并断开
  3. 高于 relay 版本的 HELLO 按向前兼容接受（未知帧由对端降级）
  4. 帧头版本不一致直接断开（帧格式不兼容）

只用最小原始帧，不依赖 agent/client，方便在 CI 里跑。
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELAY = os.path.join(ROOT, "build", "bin", "rtunnel-relay")
FRAME_HPP = os.path.join(ROOT, "rtunnel", "rtnp", "frame.hpp")

HDR = struct.Struct(">4sBBHIHH")
HELLO, HELLO_ACK, AUTH_FAIL = 0x01, 0x02, 0x05


def proto_versions():
    """从 frame.hpp 读 PROTO_VERSION / PROTO_MIN_VERSION，避免测试里写死。"""
    vals = {}
    with open(FRAME_HPP, encoding="utf-8") as fp:
        for line in fp:
            for key in ("VERSION", "PROTO_VERSION", "PROTO_MIN_VERSION"):
                if line.strip().startswith("constexpr int") and key in line and key not in vals:
                    try:
                        vals[key] = int(line.split("=", 1)[1].split(";")[0].strip())
                    except (IndexError, ValueError):
                        pass
    return vals


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def enc_str(text):
    raw = text.encode()
    return struct.pack(">H", len(raw)) + raw


def build(frame_ver, ftype, sid=0, payload=b"", flags=0):
    return HDR.pack(b"RTNP", frame_ver, ftype, flags, sid, len(payload), 0) + payload


def hello(proto, frame_ver=1):
    payload = struct.pack(">BB", 1, proto) + enc_str("compat/1") + struct.pack(">I", 0)
    return build(frame_ver, HELLO, 0, payload, 0)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_frame(sock):
    hdr = recv_exact(sock, HDR.size)
    if hdr is None:
        return None
    magic, ver, ftype, flags, sid, length, _ = HDR.unpack(hdr)
    if magic != b"RTNP":
        raise AssertionError("bad magic %r" % magic)
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    return {"ver": ver, "type": ftype, "flags": flags, "sid": sid, "payload": payload}


def connect(port, timeout=3.0):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def expect_closed(sock, deadline=1.5):
    """吞掉断开前可能先到的 ERROR/AUTH_FAIL，返回 True 表示对端已断开。"""
    end = time.time() + deadline
    while time.time() < end:
        try:
            data = sock.recv(256)
        except socket.timeout:
            return False
        except OSError:
            return True
        if data == b"":
            return True
    return False


def main():
    if not os.path.isfile(RELAY):
        print("缺少 %s，先构建" % RELAY, file=sys.stderr)
        return 2
    vers = proto_versions()
    proto = vers.get("PROTO_VERSION")
    proto_min = vers.get("PROTO_MIN_VERSION")
    if proto is None or proto_min is None:
        print("无法从 frame.hpp 读取协议版本", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="rtunnel-compat.")
    port = free_port()
    relay = subprocess.Popen(
        [RELAY, "--host", "127.0.0.1", "--port", str(port),
         "--admin", "127.0.0.1:%d" % free_port(),
         "--admin-token", "t0k", "--admin-db", os.path.join(tmp, "a.db"),
         "--admin-file-dir", tmp],
        stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "relay.log"), "wb"))
    time.sleep(0.6)

    failed = 0
    passed = 0

    def check(name, ok, extra=None):
        nonlocal failed, passed
        if ok:
            passed += 1
            print("ok   " + name)
        else:
            failed += 1
            print("FAIL " + name + ("" if extra is None else "  " + repr(extra)))

    try:
        # 1. 当前版本：HELLO_ACK 带回版本范围
        s = connect(port)
        s.sendall(hello(proto))
        f = recv_frame(s)
        check("当前版本收到 HELLO_ACK", f is not None and f["type"] == HELLO_ACK, f and f["type"])
        tail = None
        if f and f["type"] == HELLO_ACK and len(f["payload"]) >= 9 + 2:
            tail = (f["payload"][-2], f["payload"][-1])
        check("HELLO_ACK 尾部带 (proto, proto_min)", tail == (proto, proto_min), tail)
        s.close()

        # 2. 低于下限：拒绝
        s = connect(port)
        s.sendall(hello(proto_min - 1))
        f = recv_frame(s)
        check("低于下限的 HELLO 被拒绝", f is not None and f["type"] == AUTH_FAIL, f and f["type"])
        check("拒绝后断开", expect_closed(s))
        s.close()

        # 3. 高于 relay：接受（向前兼容），并回报自己的版本
        s = connect(port)
        s.sendall(hello(proto + 1))
        f = recv_frame(s)
        check("更高版本被接受", f is not None and f["type"] == HELLO_ACK, f and f["type"])
        if f and f["type"] == HELLO_ACK and len(f["payload"]) >= 9 + 2:
            check("回报 relay 自己的版本", f["payload"][-2] == proto, f["payload"][-2])
        s.close()

        # 4. 帧头版本不一致：直接断开
        s = connect(port)
        s.sendall(hello(proto, frame_ver=vers.get("VERSION", 1) + 1))
        check("帧头版本不一致时断开", expect_closed(s))
        s.close()

        # 5. 正常 agent 仍可握手认证（回归：版本改动不影响主路径）
        s = connect(port)
        s.sendall(hello(proto))
        f = recv_frame(s)
        check("主路径握手正常", f is not None and f["type"] == HELLO_ACK, f and f["type"])
        s.close()
    finally:
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            relay.kill()

    print("passed=%d failed=%d" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
