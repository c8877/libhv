#!/usr/bin/env python3
"""rtunnel 端到端验收：echo 服务 + B/C/A 三进程 + SOCKS5 客户端。"""

import os
import signal
import socket
import struct
import shutil
import subprocess
import sys
import threading
import http.cookiejar
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.environ.get("RTUNNEL_BIN", os.path.join(ROOT, "build", "bin"))
LIB = os.environ.get("RTUNNEL_LIB", os.path.join(ROOT, "build", "lib"))

results = []


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class EchoServer(threading.Thread):
    """把收到的字节原样回写，读到 EOF 后关闭（用于半关闭测试）。"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(64)
        self.port = port
        self.stop = False

    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                c.sendall(d)
        except OSError:
            pass
        finally:
            try:
                c.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            c.close()

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError("eof")
        buf += d
    return buf


def socks_connect(ep, timeout):
    if ep[0] == "unix":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(ep[1])
        return s
    return socket.create_connection((ep[1], ep[2]), timeout)


def socks5_open(ep, devid, dport, timeout=8):
    s = socks_connect(ep, timeout)
    s.settimeout(timeout)
    s.sendall(b"\x05\x01\x00")
    if recv_exact(s, 2) != b"\x05\x00":
        s.close()
        return None, -1
    d = devid.encode()
    s.sendall(b"\x05\x01\x00\x03" + bytes([len(d)]) + d + struct.pack("!H", dport))
    ver, rep, rsv, atyp = recv_exact(s, 4)
    if rep != 0:
        s.close()
        return None, rep
    if atyp == 1:
        recv_exact(s, 4 + 2)
    elif atyp == 3:
        n = recv_exact(s, 1)[0]
        recv_exact(s, n + 2)
    elif atyp == 4:
        recv_exact(s, 16 + 2)
    return s, 0


def rtnp_hdr(ftype, sid, payload=b"", flags=0):
    return (b"RTNP" + bytes([1, ftype]) + struct.pack("!H", flags) +
            struct.pack("!I", sid) + struct.pack("!HH", len(payload), 0) + payload)


def rtnp_str(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def rtnp_recv(s):
    h = recv_exact(s, 16)
    if h[:4] != b"RTNP":
        raise RuntimeError("bad magic")
    ftype = h[5]
    ln = struct.unpack("!H", h[12:14])[0]
    return ftype, (recv_exact(s, ln) if ln else b"")


class CloserServer(threading.Thread):
    """接受连接后发一句 bye 再关（验证 C→A 方向的 CLOSE 传播）。"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(16)
        self.port = port
        self.stop = False

    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                c.sendall(b"bye")
                c.shutdown(socket.SHUT_WR)
                time.sleep(0.05)
            except OSError:
                pass
            c.close()

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def _openssl(*args):
    subprocess.check_call(["openssl"] + list(args),
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_client_cert(certdir, name, san):
    """用 certdir/ca.crt 签发一张带 SAN 的客户端证书。"""
    ca_crt = os.path.join(certdir, "ca.crt")
    ca_key = os.path.join(certdir, "ca.key")
    key = os.path.join(certdir, name + ".key")
    csr = os.path.join(certdir, name + ".csr")
    crt = os.path.join(certdir, name + ".crt")
    ext = os.path.join(certdir, name + ".ext")
    with open(ext, "w") as f:
        f.write("subjectAltName=DNS:%s\n" % san)
    _openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=" + san,
             "-keyout", key, "-out", csr)
    _openssl("x509", "-req", "-in", csr, "-CA", ca_crt, "-CAkey", ca_key,
             "-CAcreateserial", "-days", "1", "-extfile", ext, "-out", crt)
    return crt, key


def wait_log(path, needle, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path, "r", errors="ignore") as f:
                if needle in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    return False


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + name + (("  " + detail) if detail and not cond else ""))


def main():
    echo_port = free_port()
    close_port = free_port()
    relay_port = free_port()
    uds = os.environ.get("RTUNNEL_UDS") == "1"
    if uds:
        socks1 = ("unix", "/tmp/rtunnel-e2e/socks1.sock")
        socks2 = ("unix", "/tmp/rtunnel-e2e/socks2.sock")
        socks1_arg, socks2_arg = "unix:" + socks1[1], "unix:" + socks2[1]
    else:
        socks1 = ("tcp", "127.0.0.1", free_port())
        socks2 = ("tcp", "127.0.0.1", free_port())
        socks1_arg, socks2_arg = "127.0.0.1:%d" % socks1[2], "127.0.0.1:%d" % socks2[2]

    echo = EchoServer(echo_port)
    echo.start()
    closer = CloserServer(close_port)
    closer.start()

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIB + ":" + env.get("LD_LIBRARY_PATH", "")

    logdir = "/tmp/rtunnel-e2e"
    os.makedirs(logdir, exist_ok=True)
    logs = {k: open(os.path.join(logdir, k + ".log"), "w") for k in ("relay", "agent", "client", "client2")}

    tls = os.environ.get("RTUNNEL_TLS") == "1"
    mtls = os.environ.get("RTUNNEL_MTLS") == "1"

    # admin 签发用 CA（mTLS 模式下与设备认证 CA 同源）
    adminca = os.path.join(logdir, "adminca")
    os.makedirs(adminca, exist_ok=True)
    admin_ca_crt = os.path.join(adminca, "ca.crt")
    admin_ca_key = os.path.join(adminca, "ca.key")
    if not os.path.exists(admin_ca_crt):
        _openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=rtunnel-admin-ca", "-keyout", admin_ca_key, "-out", admin_ca_crt)
    certout = os.path.join(logdir, "certs")
    shutil.rmtree(certout, ignore_errors=True)   # 台账/revoked 每次重跑清空
    os.makedirs(certout, exist_ok=True)
    filedir = os.path.join(logdir, "files")
    shutil.rmtree(filedir, ignore_errors=True)
    os.makedirs(filedir, exist_ok=True)
    tls_relay, tls_agent, tls_tester, tls_bob = [], [], [], []
    if tls or mtls:
        certdir = os.path.join(logdir, "cert")
        shutil.rmtree(certdir, ignore_errors=True)
        os.makedirs(certdir, exist_ok=True)
        ca_crt = os.path.join(certdir, "ca.crt")
        ca_key = os.path.join(certdir, "ca.key")
        server_crt = os.path.join(certdir, "server.crt")
        server_key = os.path.join(certdir, "server.key")
        if mtls:
            if not os.path.exists(ca_crt):
                shutil.copy(admin_ca_crt, ca_crt)
                shutil.copy(admin_ca_key, ca_key)
                make_client_cert(certdir, "server", "localhost")
                os.replace(os.path.join(certdir, "server.crt"), server_crt)
                os.replace(os.path.join(certdir, "server.key"), server_key)
            tls_relay = ["--tls-cert", server_crt, "--tls-key", server_key, "--tls-ca", ca_crt]
            tls_agent = ["--tls-ca", ca_crt, "--tls-sni", "localhost",
                         "--tls-cert", os.path.join(certdir, "agent.crt"),
                         "--tls-key", os.path.join(certdir, "agent.key")]

            def user_cert(name):
                return ["--tls-ca", ca_crt, "--tls-sni", "localhost",
                        "--tls-cert", os.path.join(certdir, name + ".crt"),
                        "--tls-key", os.path.join(certdir, name + ".key")]

            tls_tester = user_cert("tester")
            tls_bob = user_cert("bob")
            if not os.path.exists(os.path.join(certdir, "agent.crt")):
                make_client_cert(certdir, "agent", "dev1")
                make_client_cert(certdir, "tester", "tester")
                make_client_cert(certdir, "bob", "bob")
                make_client_cert(certdir, "admin", "admin")
        else:
            if not os.path.exists(server_crt):
                _openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                         "-subj", "/CN=localhost", "-keyout", server_key, "-out", server_crt)
                subprocess.check_call(["cp", server_crt, ca_crt])
            tls_relay = ["--tls-cert", server_crt, "--tls-key", server_key]
            tls_agent = ["--tls-ca", ca_crt, "--tls-sni", "localhost"]
            tls_tester = tls_bob = ["--tls-ca", ca_crt, "--tls-sni", "localhost"]

    procs = []
    admin_port = free_port()
    aclfile = os.path.join(logdir, "acl.txt")
    with open(aclfile, "w") as f:
        f.write("# initial: no file-based acl\n")
    clientusers = os.path.join(logdir, "client_users.txt")
    with open(clientusers, "w") as f:
        f.write("# user:token\ntester:tok-tester\nbob:tok-bob\nadmin:tok-admin\n")
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--acl", "tester=*", "--acl", "admin=a:*", "--acl-file", aclfile, "--drain-timeout", "500",
             "--client-users", clientusers, "--shell-users", "admin,tester",
             "--admin", "127.0.0.1:%d" % admin_port,
             "--admin-token", "admintok",
             "--admin-db", os.path.join(logdir, "admin.db"),
             "--admin-ca-cert", admin_ca_crt, "--admin-ca-key", admin_ca_key,
             "--admin-cert-dir", certout, "--admin-file-dir", filedir] + tls_relay + ["--verbose"],
            stdout=logs["relay"], stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.3)

        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", "%d,%d" % (echo_port, close_port),
             "--allow-shell"] + tls_agent + ["--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)

        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--user", "tester", "--token", "tok-tester", "--allow-shell",
             "--agent-idle", "5", "--socks5", socks1_arg] + tls_tester + ["--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
        procs.append(client)

        client2 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--user", "bob", "--token", "tok-bob", "--socks5", socks2_arg] + tls_bob + ["--verbose"],
            stdout=logs["client2"], stderr=subprocess.STDOUT, env=env)
        procs.append(client2)

        ok = (wait_log(os.path.join(logdir, "agent.log"), "authenticated")
              and wait_log(os.path.join(logdir, "client.log"), "authenticated as tester", 10)
              and wait_log(os.path.join(logdir, "client2.log"), "authenticated as bob", 10))
        check("进程就绪（agent/client 上线）", ok)
        if not ok:
            return finish()

        # 0. 认证负向：relay 配了 --client-users，无 token 的 client 不能上线
        bad_port = free_port()
        bad = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--user", "tester", "--socks5", "127.0.0.1:%d" % bad_port] + tls_tester + ["--verbose"],
            stdout=open(os.path.join(logdir, "client_bad.log"), "w"), stderr=subprocess.STDOUT, env=env)
        procs.append(bad)
        time.sleep(0.8)
        _, rep = socks5_open(("tcp", "127.0.0.1", bad_port), "dev1", echo_port)
        check("client 无 token 被拒（REP=0x01）", rep == 0x01, "rep=0x%02x" % rep)

        # 1. 基本回环
        s, rep = socks5_open(socks1, "dev1", echo_port)
        if s is None:
            check("CONNECT dev1 建链", False, "rep=%d" % rep)
        else:
            check("CONNECT dev1 建链", True)
            s.sendall(b"hello-rtunnel")
            try:
                data = recv_exact(s, len(b"hello-rtunnel"))
                check("数据回环", data == b"hello-rtunnel", repr(data))
            except Exception as e:
                check("数据回环", False, str(e))
            s.close()

        # 2. 设备离线
        _, rep = socks5_open(socks1, "ghost", echo_port)
        check("离线设备 REP=0x04", rep == 0x04, "rep=0x%02x" % rep)

        # 3. 端口白名单
        _, rep = socks5_open(socks1, "dev1", 80)
        check("端口白名单 REP=0x02", rep == 0x02, "rep=0x%02x" % rep)

        # 4. ACL（bob 无权）
        wait_log(os.path.join(logdir, "client2.log"), "authenticated", 5)
        _, rep = socks5_open(socks2, "dev1", echo_port)
        check("ACL 拒绝 REP=0x02", rep == 0x02, "rep=0x%02x" % rep)

        # 5. 并发 + 两个 A 客户端同 sid
        errs = []
        def worker(ep, tag):
            try:
                s, rep = socks5_open(ep, "dev1", echo_port)
                if s is None:
                    raise RuntimeError("rep=%d" % rep)
                for i in range(20):
                    msg = ("%s-%d" % (tag, i)).encode()
                    s.sendall(msg)
                    if recv_exact(s, len(msg)) != msg:
                        raise RuntimeError("mismatch")
                s.close()
            except Exception as e:
                errs.append("%s: %s" % (tag, e))

        threads = [threading.Thread(target=worker, args=(socks1, "a")),
                   threading.Thread(target=worker, args=(socks1, "b")),
                   threading.Thread(target=worker, args=(socks2, "c"))]
        # bob 无权限，c 预期失败；只校验 a/b
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        check("并发 3 会话（2 个 A 客户端同 sid 不串流）",
              not [e for e in errs if e.startswith(("a:", "b:"))], "; ".join(errs))

        # 6. 设备端关闭传播：sshd 侧发完数据就关，A 应转发 FIN 给 SOCKS 客户端
        s, rep = socks5_open(socks1, "dev1", close_port)
        if s is None:
            check("设备端关闭传播", False, "rep=%d" % rep)
        else:
            got = b""
            eof = False
            try:
                while True:
                    d = s.recv(4096)
                    if not d:
                        eof = True
                        break
                    got += d
            except Exception:
                pass
            check("设备端关闭传播（数据 + FIN）", got == b"bye" and eof,
                  "got=%r eof=%s" % (got, eof))
            s.close()

        # 6.4 CLI 免密 shell：远端 shell 退出后客户端必须退出
        try:
            r = subprocess.run(
                [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
                 "--user", "tester", "--token", "tok-tester", "shell", "dev1"] + tls_tester,
                input=b"echo CLI-SHELL-$((6*7)); exit\n", capture_output=True, timeout=10)
            out = (r.stdout + r.stderr).decode(errors="ignore")
            check("CLI 免密 shell 正常退出", r.returncode == 0 and "CLI-SHELL-42" in out, out[-120:])
        except subprocess.TimeoutExpired:
            check("CLI 免密 shell 正常退出", False, "timeout")

        # 6.5 A 侧按需 agent：admin 对 a:tester 发 OPEN_SHELL，relay 通知 client 拉起本地 agent
        try:
            q = socket.create_connection(("127.0.0.1", relay_port), 5)
            q.settimeout(15)
            if tls or mtls:
                import ssl as ssl_mod
                ctx = ssl_mod.create_default_context(cafile=ca_crt)
                if mtls:
                    ctx.load_cert_chain(os.path.join(certdir, "admin.crt"),
                                        os.path.join(certdir, "admin.key"))
                q = ctx.wrap_socket(q, server_hostname="localhost")
            q.sendall(rtnp_hdr(0x01, 0, bytes([2, 1]) + rtnp_str("e2e") + struct.pack("!I", 0)))
            t, _ = rtnp_recv(q)
            q.sendall(rtnp_hdr(0x03, 0, rtnp_str("admin") + b"\x01" + rtnp_str("tok-admin")))
            t, _ = rtnp_recv(q)      # AUTH_OK
            q.sendall(rtnp_hdr(0x10, 9, rtnp_str("a:tester") + struct.pack("!H", 0), flags=1))
            ok = False
            deadline = time.time() + 12
            while time.time() < deadline:
                t, _ = rtnp_recv(q)
                if t == 0x11:
                    ok = True
                    break
                if t in (0x12, 0x16):
                    break
            check("A 侧按需 agent（OPEN_SHELL a:tester）", ok)
            q.close()
        except Exception as e:
            check("A 侧按需 agent（OPEN_SHELL a:tester）", False, repr(e))

        # 7. 大流量（256 KiB）
        s, rep = socks5_open(socks1, "dev1", echo_port, timeout=20)
        total = 256 * 1024
        if s is None:
            check("大流量 256KiB", False, "rep=%d" % rep)
        else:
            payload = bytes((i * 7 + 3) & 0xFF for i in range(total))

            def sender():
                s.sendall(payload)

            th = threading.Thread(target=sender)
            th.start()
            recv = b""
            try:
                while len(recv) < total:
                    d = s.recv(65536)
                    if not d:
                        break
                    recv += d
            except Exception:
                pass
            th.join()
            if recv != payload:
                idx = next((i for i in range(min(len(recv), len(payload))) if recv[i] != payload[i]), -1)
                check("大流量 256KiB 回环完整", False,
                      "%d/%d first-diff=%d got=%s want=%s" %
                      (len(recv), total, idx, recv[max(idx - 2, 0):idx + 6].hex(),
                       payload[max(idx - 2, 0):idx + 6].hex()))
            else:
                check("大流量 256KiB 回环完整", True)
            s.close()

        # 8. OPEN 未决时断开：CANCELLED 清理（relay 映射不悬挂）
        def open_then_abort(n):
            for _ in range(n):
                try:
                    s = socks_connect(socks1, 5)
                    s.settimeout(5)
                    s.sendall(b"\x05\x01\x00")
                    recv_exact(s, 2)
                    d = b"dev1"
                    s.sendall(b"\x05\x01\x00\x03" + bytes([len(d)]) + d + struct.pack("!H", echo_port))
                    s.close()   # 不等 REP：部分会话落在 OPENING 窗口
                except OSError:
                    pass
                time.sleep(0.01)
        open_then_abort(30)
        time.sleep(0.5)

        # 原始 RTNP 连接查 DEVICE_QUERY：active_streams 必须归零
        q = socket.create_connection(("127.0.0.1", relay_port), 5)
        q.settimeout(5)
        if tls or mtls:
            import ssl as ssl_mod
            ctx = ssl_mod.create_default_context(cafile=ca_crt)
            if mtls:
                ctx.load_cert_chain(os.path.join(certdir, "tester.crt"),
                                    os.path.join(certdir, "tester.key"))
            q = ctx.wrap_socket(q, server_hostname="localhost")
        try:
            q.sendall(rtnp_hdr(0x01, 0, bytes([2, 1]) + rtnp_str("e2e-query") + struct.pack("!I", 0)))
            t, _ = rtnp_recv(q)          # HELLO_ACK
            if t != 0x02:
                raise RuntimeError("no HELLO_ACK: 0x%02x" % t)
            q.sendall(rtnp_hdr(0x03, 0, rtnp_str("tester") + b"\x01" + rtnp_str("tok-tester")))
            t, _ = rtnp_recv(q)          # AUTH_OK
            if t != 0x04:
                raise RuntimeError("no AUTH_OK: 0x%02x" % t)
            q.sendall(rtnp_hdr(0x08, 0, rtnp_str("dev1")))
            t, p = rtnp_recv(q)          # DEVICE_STATUS
            if t != 0x09:
                raise RuntimeError("no DEVICE_STATUS: 0x%02x" % t)
            dlen = struct.unpack("!H", p[:2])[0]
            online = p[2 + dlen]
            active = struct.unpack("!I", p[2 + dlen + 1:2 + dlen + 5])[0]
            check("OPEN 未决断开 CANCELLED 不悬挂（active_streams=0）",
                  online == 1 and active == 0,
                  "online=%d active=%d" % (online, active))
        except Exception as e:
            check("OPEN 未决断开 CANCELLED 不悬挂（active_streams=0）", False, str(e))
        finally:
            q.close()

        # 9. SIGHUP 热加载 ACL：bob 从拒绝→放行→再拒绝
        with open(aclfile, "w") as f:
            f.write("bob=dev1\n")
        os.kill(relay.pid, signal.SIGHUP)
        time.sleep(0.2)
        s, rep = socks5_open(socks2, "dev1", echo_port)
        check("SIGHUP 热加载后 bob 放行", rep == 0x00, "rep=0x%02x" % rep)
        if s:
            s.close()
        with open(aclfile, "w") as f:
            f.write("# revoked\n")
        os.kill(relay.pid, signal.SIGHUP)
        time.sleep(0.2)
        _, rep = socks5_open(socks2, "dev1", echo_port)
        check("SIGHUP 撤销后 bob 再拒", rep == 0x02, "rep=0x%02x" % rep)

        # 10. admin 管理台只读 API 与前端
        import json as json_mod
        import urllib.request
        admin_base = "http://127.0.0.1:%d" % admin_port

        def make_admin(base, token="admintok"):
            jar = http.cookiejar.CookieJar()
            op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
            def call(method, path, body=None):
                data = json_mod.dumps(body).encode() if body is not None else None
                req = urllib.request.Request(base + path, data=data, method=method)
                if data:
                    req.add_header("Content-Type", "application/json")
                with op.open(req, timeout=5) as r:
                    return json_mod.loads(r.read().decode())
            for path, body in (
                    ("/api/v1/setup", {"token": token, "username": "root", "password": "rootpass1"}),
                    ("/api/v1/login", {"username": "root", "password": "rootpass1"})):
                req = urllib.request.Request(base + path, data=json_mod.dumps(body).encode(), method="POST")
                req.add_header("Content-Type", "application/json")
                op.open(req, timeout=5).read()
            return op, call

        admin_opener, admin_call = make_admin(admin_base)

        def admin_get(path, token=None):
            return admin_call("GET", path)

        try:
            ov = admin_get("/api/v1/overview")
            check("admin overview", ov.get("agents") >= 1 and ov.get("clients") == 2, str(ov))
            ag = admin_get("/api/v1/agents")
            check("admin agents 列表", any(a["devid"] == "dev1" for a in ag), str(ag))
            cl = admin_get("/api/v1/clients")
            check("admin clients 列表", {c["user"] for c in cl} == {"tester", "bob"}, str(cl))
            d = admin_get("/api/v1/agents/dev1")
            check("admin 设备详情", d.get("devid") == "dev1" and "streams" in d, str(d)[:120])
            s, rep = socks5_open(socks1, "dev1", echo_port)
            st = admin_get("/api/v1/streams")
            check("admin streams 列表",
                  any(x["c_devid"] == "dev1" and x["state"] == "OPEN" for x in st), str(st))
            if s:
                s.close()
            with urllib.request.urlopen(admin_base + "/", timeout=5) as r:
                html = r.read().decode()
            check("admin 前端页面", "RTUNNEL" in html and "/admin.js" in html)
            with urllib.request.urlopen(admin_base + "/admin.js", timeout=5) as r:
                js = r.read().decode()
            check("admin.js 可访问", "Router" in js)
            with urllib.request.urlopen(admin_base + "/admin.css", timeout=5) as r:
                css = r.read().decode()
            check("admin.css 可访问", "sidebar" in css)
        except Exception as e:
            check("admin 管理台", False, str(e))

        # 11. admin 操作面：踢会话 / 设备下线重连 / ACL 读写
        def admin_req(method, path, body=None, token=None):
            return admin_call(method, path, body)

        s, rep = socks5_open(socks1, "dev1", echo_port)
        st = admin_get("/api/v1/streams")
        kicked = False
        if st:
            a_sid = st[0]["a_sid"]
            r = admin_req("DELETE", "/api/v1/streams/%d" % a_sid)
            try:
                eof = (s.recv(4096) == b"")
            except OSError:
                eof = True
            kicked = bool(r.get("kicked")) and eof
        check("admin 踢会话（双侧 RESET）", kicked)
        if s:
            s.close()
        time.sleep(0.2)
        st = admin_get("/api/v1/streams")
        check("踢后 streams 为空", len(st) == 0, str(st))

        admin_req("PUT", "/api/v1/acl/file", {"content": "bob=dev1\n"})
        _, rep = socks5_open(socks2, "dev1", echo_port)
        check("ACL PUT 后 bob 放行", rep == 0x00, "rep=0x%02x" % rep)
        admin_req("PUT", "/api/v1/acl/file", {"content": "# empty\n"})
        _, rep = socks5_open(socks2, "dev1", echo_port)
        check("ACL PUT 撤销后 bob 再拒", rep == 0x02, "rep=0x%02x" % rep)

        admin_req("POST", "/api/v1/agents/dev1/kick")
        # 等设备重新注册上线（kick 后 agent 退避重连，轮询最多 8s）
        rep = -1
        s = None
        for _ in range(16):
            time.sleep(0.5)
            s, rep = socks5_open(socks1, "dev1", echo_port)
            if rep == 0x00:
                break
        check("agent 被踢后自动重连", rep == 0x00, "rep=0x%02x" % rep)
        if s:
            s.close()

        # 12. admin 证书面（relay 未编译 OpenSSL 时跳过）
        cert_ok = True
        try:
            admin_req("POST", "/api/v1/certs/sign", {"csr": "not-a-csr", "days": 1})
        except urllib.error.HTTPError as e:
            if e.code == 501:
                cert_ok = False
                print("SKIP 证书面（relay 未编译 OpenSSL）")
            # 400 = 接口存在但参数不合法，说明支持签发
        if cert_ok:
            # 12. admin 证书面：签发 / 吊销 / 审计
            r = admin_req("POST", "/api/v1/certs/issue", {"san": "PILE-900001", "role": "device", "days": 90})
            check("签发证书", r.get("san") == "PILE-900001" and "cert" in r and "key" in r)
            newcrt = os.path.join(certout, "PILE-900001.crt")
            newkey = os.path.join(certout, "PILE-900001.key")
            check("证书文件落盘", os.path.exists(newcrt) and os.path.exists(newkey))
            cl = admin_get("/api/v1/certs")
            check("证书台账", any(c["san"] == "PILE-900001" and c["status"] == "valid" for c in cl), str(cl))

            if mtls:
                # 新证书可注册上线（与 relay 的认证 CA 同源）
                new_log = open(os.path.join(logdir, "agent900001.log"), "w")
                new_agent = subprocess.Popen(
                    [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
                     "--devid", "PILE-900001", "--allow", str(echo_port),
                     "--tls-ca", os.path.join(certdir, "ca.crt"), "--tls-sni", "localhost",
                     "--tls-cert", newcrt, "--tls-key", newkey, "--verbose"],
                    stdout=new_log, stderr=subprocess.STDOUT, env=env)
                procs.append(new_agent)
                ok = wait_log(os.path.join(logdir, "agent900001.log"), "authenticated as PILE-900001", 8)
                check("签发证书可注册上线", ok)

            admin_req("POST", "/api/v1/certs/revoke", {"san": "PILE-900001"})
            cl = admin_get("/api/v1/certs")
            check("吊销状态", any(c["san"] == "PILE-900001" and c["status"] == "revoked" for c in cl), str(cl))
            if mtls:
                ok = wait_log(os.path.join(logdir, "relay.log"), "identity revoked", 8)
                check("吊销后 AUTH 被拒", ok)

            # CSR 上传签名（私钥不出设备；无 SAN 的 CSR 走 CN 回退）
            csr_key = os.path.join(certout, "PILE-900002.key")
            csr_file = os.path.join(certout, "PILE-900002.csr")
            _openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=PILE-900002",
                     "-keyout", csr_key, "-out", csr_file)
            with open(csr_file) as f:
                csr_pem = f.read()
            r = admin_req("POST", "/api/v1/certs/sign", {"csr": csr_pem, "days": 90})
            check("CSR 签名", r.get("san") == "PILE-900002" and "cert" in r, str(r)[:100])
            if mtls:
                with open(os.path.join(certout, "PILE-900002.crt"), "w") as f:
                    f.write(r["cert"])
                new_log2 = open(os.path.join(logdir, "agent900002.log"), "w")
                new_agent2 = subprocess.Popen(
                    [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
                     "--devid", "PILE-900002", "--allow", str(echo_port),
                     "--tls-ca", os.path.join(certdir, "ca.crt"), "--tls-sni", "localhost",
                     "--tls-cert", os.path.join(certout, "PILE-900002.crt"), "--tls-key", csr_key, "--verbose"],
                    stdout=new_log2, stderr=subprocess.STDOUT, env=env)
                procs.append(new_agent2)
                ok = wait_log(os.path.join(logdir, "agent900002.log"), "authenticated as PILE-900002", 8)
                check("CSR 签出证书可注册上线", ok)

            metrics = admin_opener.open(admin_base + "/metrics", timeout=5).read().decode()
            check("/metrics", "rtunnel_agents" in metrics and "rtunnel_bytes_in_total" in metrics)

            # 文件管理：上传/列表/排序/下载/删除 + 角色权限
            def upload(name, data, token=None):
                req = urllib.request.Request(admin_base + "/api/v1/files/" + name, data=data, method="POST")
                with admin_opener.open(req, timeout=5) as r:
                    return json_mod.loads(r.read().decode())

            upload("a-small.txt", b"hello")
            upload("b-large.bin", bytes(range(256)) * 8)   # 2048B
            fl = admin_get("/api/v1/files?sort=size&order=desc")
            check("文件列表+按大小降序", len(fl) == 2 and fl[0]["name"] == "b-large.bin", str(fl))
            fl = admin_get("/api/v1/files?sort=name&order=asc")
            check("按名称升序", fl[0]["name"] == "a-small.txt", str(fl))
            with admin_opener.open(admin_base + "/api/v1/files/b-large.bin", timeout=5) as r:
                data = r.read()
            check("下载内容一致", data == bytes(range(256)) * 8)
            admin_req("DELETE", "/api/v1/files/a-small.txt")
            fl = admin_get("/api/v1/files")
            check("删除后列表", len(fl) == 1 and fl[0]["name"] == "b-large.bin", str(fl))
            try:
                upload("..%2Fescape", b"x")
                check("目录穿越被拒", False, "unexpected success")
            except urllib.error.HTTPError as e:
                check("目录穿越被拒", e.code in (400, 404), "code=%d" % e.code)

            au = admin_get("/api/v1/audit")
            check("审计日志", any("certs.issue" in x for x in au) and any("certs.revoke" in x for x in au), str(au)[:120])

        # 13. mTLS 负向：证书 SAN 与 devid 不符必须被拒
        if mtls:
            rogue_crt, rogue_key = make_client_cert(certdir, "rogue", "dev2")
            rogue_log = open(os.path.join(logdir, "rogue.log"), "w")
            rogue = subprocess.Popen(
                [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
                 "--devid", "dev1", "--allow", str(echo_port),
                 "--tls-ca", os.path.join(certdir, "ca.crt"), "--tls-sni", "localhost",
                 "--tls-cert", rogue_crt, "--tls-key", rogue_key, "--verbose"],
                stdout=rogue_log, stderr=subprocess.STDOUT, env=env)
            procs.append(rogue)
            ok = wait_log(os.path.join(logdir, "relay.log"), "SAN mismatch", 8)
            check("mTLS: 证书 SAN 与 devid 不符被拒", ok)
            rogue.terminate()
            rogue_log.close()

            # 9. mTLS 负向：客户端证书 SAN 与 user 不符必须被拒（bob 的证书声称 tester）
            rogue_c_log = open(os.path.join(logdir, "rogue_client.log"), "w")
            rogue_c = subprocess.Popen(
                [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
                 "--user", "tester", "--token", "tok-tester", "--socks5", "127.0.0.1:%d" % free_port()] + tls_bob + ["--verbose"],
                stdout=rogue_c_log, stderr=subprocess.STDOUT, env=env)
            procs.append(rogue_c)
            ok = wait_log(os.path.join(logdir, "relay.log"), "client cert SAN mismatch", 8)
            check("mTLS: 客户端证书 SAN 与 user 不符被拒", ok)
            rogue_c.terminate()
            rogue_c_log.close()

        # admin /shutdown（独立 relay 实例，与 SIGTERM 路径分别覆盖）
        r2_port = free_port()
        a2_port = free_port()
        r2log = open(os.path.join(logdir, "relay2.log"), "w")
        relay2 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(r2_port),
             "--acl", "tester=*", "--drain-timeout", "200",
             "--admin", "127.0.0.1:%d" % a2_port, "--admin-token", "admintok",
             "--admin-db", os.path.join(logdir, "admin2.db")],
            stdout=r2log, stderr=subprocess.STDOUT, env=env)
        procs.append(relay2)
        time.sleep(0.5)
        _, call2 = make_admin("http://127.0.0.1:%d" % a2_port)
        resp = call2("POST", "/api/v1/shutdown", {})
        try:
            rc2 = relay2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc2 = None
        check("admin /shutdown 优雅退出", resp.get("draining") and rc2 == 0, "rc=%s" % rc2)
        r2log.close()

        # 11. SIGTERM 优雅退出（drain 500ms 后强制退出，进程 rc=0）
        relay.send_signal(signal.SIGTERM)
        ok = wait_log(os.path.join(logdir, "relay.log"), "draining", 5)
        try:
            rc = relay.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = None
        check("SIGTERM 优雅退出", ok and rc == 0, "draining=%s rc=%s" % (ok, rc))

    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        echo.close()
        closer.close()
        for f in logs.values():
            f.flush()
            f.close()

    return finish()


def finish():
    failed = [r for r in results if not r[1]]
    print("\n=== %d/%d passed ===" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
