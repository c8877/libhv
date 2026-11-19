#!/usr/bin/env python3
"""验证 relay 的流空闲回收：OPEN 后长时间无数据会被 RESET，期间有数据则保持。"""

import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.environ.get("RTUNNEL_BIN", os.path.join(ROOT, "build", "bin"))

HELLO, HELLO_ACK, AUTH, AUTH_OK = 0x01, 0x02, 0x03, 0x04
OPEN, OPEN_OK, OPEN_FAIL = 0x10, 0x11, 0x12
DATA, CLOSE, RESET = 0x13, 0x15, 0x16

failures = []


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError("eof")
        buf += d
    return buf


def hdr(ftype, sid, payload=b"", flags=0):
    return (b"RTNP" + bytes([1, ftype]) + struct.pack("!H", flags) +
            struct.pack("!I", sid) + struct.pack("!HH", len(payload), 0) + payload)


def rstr(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def recv_frame(s):
    h = recv_exact(s, 16)
    if h[:4] != b"RTNP":
        raise RuntimeError("bad magic")
    ftype = h[5]
    sid = struct.unpack("!I", h[8:12])[0]
    ln = struct.unpack("!H", h[12:14])[0]
    return ftype, sid, (recv_exact(s, ln) if ln else b"")


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("  " + detail) if detail and not cond else ""))
    if not cond:
        failures.append(name)


class Peer:
    def __init__(self, port, role, ident, token=None):
        self.s = socket.create_connection(("127.0.0.1", port), 5)
        self.s.settimeout(15)
        self.s.sendall(hdr(HELLO, 0, bytes([role, 1]) + rstr("stream-idle-test")))
        t, _, _ = recv_frame(self.s)
        if t != HELLO_ACK:
            raise RuntimeError("hello ack: 0x%02x" % t)
        if role == 1:
            self.s.sendall(hdr(AUTH, 0, rstr(ident)))
        else:
            self.s.sendall(hdr(AUTH, 0, rstr(ident) + bytes([1]) + rstr(token or "")))
        t, _, _ = recv_frame(self.s)
        if t != AUTH_OK:
            raise RuntimeError("auth: 0x%02x" % t)

    def open(self, sid, devid, port):
        self.s.sendall(hdr(OPEN, sid, rstr(devid) + struct.pack("!H", port)))

    def send_data(self, sid, data):
        self.s.sendall(hdr(DATA, sid, data))

    def wait(self, want, timeout):
        """收帧直到出现 want；返回 (type, sid)。其它帧忽略。"""
        end = time.time() + timeout
        self.s.settimeout(max(0.2, timeout))
        while time.time() < end:
            try:
                t, sid, _ = recv_frame(self.s)
            except (socket.timeout, TimeoutError):
                break
            if t == want:
                return t, sid
        return None, None

    def try_read(self, timeout):
        self.s.settimeout(timeout)
        try:
            return recv_frame(self.s)
        except (socket.timeout, TimeoutError):
            return None
        except EOFError:
            return (None, None, None)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    relay_port = free_port()
    log_path = "/tmp/rtunnel-stream-idle.log"
    log = open(log_path, "w")
    relay = subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
         "--allow-all", "--client-token", "tok1", "--stream-idle", "2", "--verbose"],
        stdout=log, stderr=subprocess.STDOUT)
    try:
        time.sleep(0.6)
        agent = Peer(relay_port, 1, "idle-dev")
        client = Peer(relay_port, 2, "idle-user", "tok1")

        # 1. 建流后不发数据，应在 idle(2s) + 扫描周期(5s) 内被 RESET
        client.open(41, "idle-dev", 1234)
        t, sid, _ = recv_frame(agent.s)
        check("agent 收到 OPEN", t == OPEN and sid != 0, "0x%02x" % t)
        agent.s.sendall(hdr(OPEN_OK, sid))
        t, sid, _ = recv_frame(client.s)
        check("client 收到 OPEN_OK", t == OPEN_OK and sid == 41, "0x%02x sid=%s" % (t, sid))

        t, sid = client.wait(RESET, 20)
        check("空闲流被 relay RESET", t == RESET and sid == 41, "t=%s sid=%s" % (t, sid))
        t, sid = agent.wait(RESET, 3)
        check("agent 侧也收到 RESET", t == RESET, "t=%s" % t)

        # 流已删除：再用旧 sid 发数据应回 unknown sid
        client.send_data(41, b"x")
        t, sid, payload = client.try_read(3)
        check("回收后旧 sid 不再可用", t == RESET and sid == 41, "t=%s sid=%s" % (t, sid))

        # 2. 持续有数据时不会被回收
        client.open(42, "idle-dev", 1234)
        t, sid2, _ = recv_frame(agent.s)
        check("第二条流 OPEN", t == OPEN, "0x%02x" % t)
        agent.s.sendall(hdr(OPEN_OK, sid2))
        t, _, _ = recv_frame(client.s)
        check("第二条流 OPEN_OK", t == OPEN_OK, "0x%02x" % t)
        for _ in range(8):
            client.send_data(42, b"ping")
            t, _, _ = recv_frame(agent.s)
            if t != DATA:
                check("活跃流转发 DATA", False, "0x%02x" % t)
                break
            time.sleep(0.8)
        else:
            check("活跃流未被回收", client.try_read(0.1) is None)

        # 停止发数据后仍会被回收
        client.send_data(42, b"last")
        recv_frame(agent.s)
        t, sid = client.wait(RESET, 20)
        check("停止活动后被回收", t == RESET and sid == 42, "t=%s sid=%s" % (t, sid))

        client.close()
        agent.close()
    finally:
        relay.terminate()
        try:
            relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            relay.kill()
        log.close()

    if failures:
        print("stream idle e2e FAILED: %s" % ", ".join(failures))
        print("--- relay log ---")
        print(open(log_path, errors="ignore").read()[-2000:])
        return 1
    print("STREAM IDLE E2E PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
