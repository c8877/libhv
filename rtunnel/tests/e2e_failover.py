#!/usr/bin/env python3
"""多 relay 故障切换 e2e：两个 relay + 一个 agent + 一个 SOCKS5 client。

场景：
  1. agent/client 都只配 r1 时一切正常（基线）
  2. 用 --server r1,r2 启动，kill r1 后 agent/client 自动切到 r2，SOCKS 仍可用
  3. r1 恢复后保持 r2（sticky，不自动切回）
  4. 两个地址都不通时继续重试（不退出）
"""

import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2e  # noqa: E402

BIN, LIB = e2e.BIN, e2e.LIB
LOG = "/tmp/rtunnel-e2e-failover"


def start_relay(port, log):
    return subprocess.Popen(
        [os.path.join(BIN, "rtunnel-relay"), "--host", "127.0.0.1", "--port", str(port),
         "--allow-all", "--drain-timeout", "300", "--verbose"],
        stdout=log, stderr=subprocess.STDOUT, env=ENV)


def echo_roundtrip(socks_ep, echo_port, timeout=10):
    s, rep = e2e.socks5_open(socks_ep, "dev1", echo_port, timeout=timeout)
    if s is None:
        return False, "socks rep=%d" % rep
    try:
        s.sendall(b"ping")
        got = e2e.recv_exact(s, 4)
        return got == b"ping", "echo=%r" % got
    except OSError as e:
        return False, str(e)
    finally:
        s.close()


def wait_roundtrip(socks_ep, echo_port, timeout):
    end = time.time() + timeout
    last = ""
    while time.time() < end:
        ok, last = echo_roundtrip(socks_ep, echo_port, timeout=3)
        if ok:
            return True, last
        time.sleep(0.3)
    return False, last


def main():
    os.makedirs(LOG, exist_ok=True)
    echo_port = e2e.free_port()
    r1_port, r2_port = e2e.free_port(), e2e.free_port()
    socks_port = e2e.free_port()
    socks_ep = ("tcp", "127.0.0.1", socks_port)

    echo = e2e.EchoServer(echo_port)
    echo.start()
    logs = {k: open(os.path.join(LOG, k + ".log"), "w")
            for k in ("relay1", "relay2", "relay1b", "agent", "client")}
    procs = []
    try:
        r1 = start_relay(r1_port, logs["relay1"])
        procs.append(r1)
        r2 = start_relay(r2_port, logs["relay2"])
        procs.append(r2)
        time.sleep(0.4)

        servers = ["--server", "127.0.0.1:%d" % r1_port,
                   "--server", "127.0.0.1:%d" % r2_port]
        agent = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-agent")] + servers +
            ["--devid", "dev1", "--allow", str(echo_port),
             "--config-file", os.path.join(LOG, "agent-config.json"), "--verbose"],
            stdout=logs["agent"], stderr=subprocess.STDOUT, env=ENV)
        procs.append(agent)
        client = subprocess.Popen(
            [os.path.join(BIN, "rtunnel-client"), "--user", "tester"] + servers +
            ["--socks5", "127.0.0.1:%d" % socks_port, "--verbose"],
            stdout=logs["client"], stderr=subprocess.STDOUT, env=ENV)
        procs.append(client)

        ok, detail = wait_roundtrip(socks_ep, echo_port, timeout=10)
        e2e.check("failover: 初始经 r1 连通", ok, detail)
        e2e.check("failover: agent 首选 r1",
                  e2e.wait_log(os.path.join(LOG, "agent.log"), "failover", timeout=0.1) is False,
                  "agent 不应在成功前切换")

        # 杀掉 r1，两端都应自动切到 r2
        r1.send_signal(signal.SIGTERM)
        ok, detail = wait_roundtrip(socks_ep, echo_port, timeout=20)
        e2e.check("failover: r1 下线后经 r2 恢复", ok, detail)
        e2e.check("failover: agent 记录了切换",
                  e2e.wait_log(os.path.join(LOG, "agent.log"), "failover", timeout=5))
        e2e.check("failover: client 记录了切换",
                  e2e.wait_log(os.path.join(LOG, "client.log"), "failover", timeout=5))

        # r1 恢复：应保持 r2（sticky）
        r1b = start_relay(r1_port, logs["relay1b"])
        procs.append(r1b)
        time.sleep(2.0)
        ok, detail = wait_roundtrip(socks_ep, echo_port, timeout=10)
        e2e.check("failover: r1 恢复后仍可用", ok, detail)
        e2e.check("failover: r1 恢复后不切回（sticky）",
                  e2e.wait_log(os.path.join(LOG, "relay1b.log"), "agent online: dev1", timeout=0.2) is False,
                  "r1 不应看到 dev1 重连")

        # 双 relay 全挂：进程不退出，继续重试
        r2.send_signal(signal.SIGTERM)
        r1b.send_signal(signal.SIGTERM)
        time.sleep(2.5)
        e2e.check("failover: 全挂时不退出", agent.poll() is None and client.poll() is None,
                  "agent=%r client=%r" % (agent.poll(), client.poll()))
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    p.send_signal(signal.SIGTERM)
                    p.wait(timeout=3)
                except (subprocess.TimeoutExpired, OSError):
                    p.kill()
        echo.close()
        for f in logs.values():
            f.flush()
            f.close()
    return e2e.finish()


ENV = dict(os.environ)
ENV["LD_LIBRARY_PATH"] = LIB + ":" + ENV.get("LD_LIBRARY_PATH", "")

if __name__ == "__main__":
    sys.exit(main())
