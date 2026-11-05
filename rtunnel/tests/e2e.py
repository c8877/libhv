#!/usr/bin/env python3
"""rtunnel 端到端验收：echo 服务 + B/C/A 三进程 + SOCKS5 客户端。"""

import os
import socket
import struct
import shutil
import subprocess
import sys
import threading
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
    tls_relay, tls_agent, tls_client = [], [], []
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
                _openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                         "-subj", "/CN=rtunnel-ca", "-keyout", ca_key, "-out", ca_crt)
                make_client_cert(certdir, "server", "localhost")
                os.replace(os.path.join(certdir, "server.crt"), server_crt)
                os.replace(os.path.join(certdir, "server.key"), server_key)
            tls_relay = ["--tls-cert", server_crt, "--tls-key", server_key, "--tls-ca", ca_crt]
            tls_agent = ["--tls-ca", ca_crt, "--tls-sni", "localhost",
                         "--tls-cert", os.path.join(certdir, "agent.crt"),
                         "--tls-key", os.path.join(certdir, "agent.key")]
            tls_client = ["--tls-ca", ca_crt, "--tls-sni", "localhost",
                          "--tls-cert", os.path.join(certdir, "alice.crt"),
                          "--tls-key", os.path.join(certdir, "alice.key")]
            if not os.path.exists(os.path.join(certdir, "agent.crt")):
                make_client_cert(certdir, "agent", "dev1")
                make_client_cert(certdir, "alice", "alice")
        else:
            if not os.path.exists(server_crt):
                _openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                         "-subj", "/CN=localhost", "-keyout", server_key, "-out", server_crt)
                subprocess.check_call(["cp", server_crt, ca_crt])
            tls_relay = ["--tls-cert", server_crt, "--tls-key", server_key]
            tls_agent = ["--tls-ca", ca_crt, "--tls-sni", "localhost"]
            tls_client = ["--tls-ca", ca_crt, "--tls-sni", "localhost"]

    procs = []
    try:
        relay = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(relay_port),
             "--acl", "tester=*"] + tls_relay + ["--verbose"],
            stdout=logs["relay"], stderr=subprocess.STDOUT, env=env)
        procs.append(relay)
        time.sleep(0.3)

        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", "dev1", "--allow", "%d,%d" % (echo_port, close_port)] + tls_agent + ["--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=env)
        procs.append(agent)

        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--user", "tester", "--socks5", socks1_arg] + tls_client + ["--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=env)
        procs.append(client)

        client2 = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--server", "127.0.0.1", "--port", str(relay_port),
             "--user", "bob", "--socks5", socks2_arg] + tls_client + ["--verbose"],
            stdout=logs["client2"], stderr=subprocess.STDOUT, env=env)
        procs.append(client2)

        ok = (wait_log(os.path.join(logdir, "agent.log"), "authenticated")
              and wait_log(os.path.join(logdir, "client.log"), "authenticated as tester", 10)
              and wait_log(os.path.join(logdir, "client2.log"), "authenticated as bob", 10))
        check("进程就绪（agent/client 上线）", ok)
        if not ok:
            return finish()

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


        # 8. mTLS 负向：证书 SAN 与 devid 不符必须被拒
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
