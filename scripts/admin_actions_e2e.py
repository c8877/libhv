#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""管理台列表行动作 Playwright 用例。

覆盖 UI.actions 迁移后的账号、文件、设备三页按钮：
创建/禁用/改角色/范围/删除账号，上传/复制/下载/删除文件，强制下线设备。

用法:
    python3 scripts/admin_actions_e2e.py [--bin DIR] [--keep]

依赖:
    pip install playwright && playwright install chromium
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sys.exit("需要 playwright：pip install playwright && playwright install chromium")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_admin(base, proc, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            with urllib.request.urlopen(base + "/api/v1/setup/status", timeout=1):
                return True
        except Exception:
            time.sleep(0.15)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "build", "bin"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    relay_bin = os.path.join(args.bin, "rtunnel-relay")
    agent_bin = os.path.join(args.bin, "rtunnel-agent")
    for p in (relay_bin, agent_bin):
        if not os.path.exists(p):
            sys.exit("找不到二进制: " + p)

    work = tempfile.mkdtemp(prefix="actions-ui-")
    relay_port, admin_port = free_port(), free_port()
    base = "http://127.0.0.1:%d" % admin_port
    log = open(os.path.join(work, "relay.log"), "w")
    procs = []
    relay = subprocess.Popen(
        [relay_bin, "--host", "127.0.0.1", "--port", str(relay_port),
         "--admin", "127.0.0.1:%d" % admin_port, "--admin-token", "boot",
         "--admin-db", os.path.join(work, "admin.db"),
         "--admin-file-dir", os.path.join(work, "files")],
        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    procs.append(relay)
    if not wait_admin(base, relay):
        sys.exit("relay 启动失败，见 " + os.path.join(work, "relay.log"))
    procs.append(subprocess.Popen(
        [agent_bin, "--server", "127.0.0.1", "--port", str(relay_port),
         "--devid", "dev1", "--allow", "22", "--config-file", os.path.join(work, "agent.json")],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True))

    browser = None
    fails = []
    total = [0]

    def check(name, ok, detail=""):
        total[0] += 1
        print(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail) + "]") if detail and not ok else ""))
        if not ok:
            fails.append(name)

    try:
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            ctx = browser.new_context(viewport={"width": 1400, "height": 900},
                                      permissions=["clipboard-read", "clipboard-write"])
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))

            page.goto(base, wait_until="networkidle")
            page.fill("#af-token", "boot")
            page.fill("#af-user", "root")
            page.fill("#af-pass", "rootpass1")
            page.click("#af-btn")
            page.wait_for_timeout(1200)

            # ---------------- 账号：创建 / 状态 / 角色 / 范围 / 删除 ----------------
            page.goto(base + "/#/accounts")
            page.wait_for_selector("#nu-open")
            page.click("#nu-open")
            page.wait_for_selector("[data-field=username]")
            page.fill("[data-field=username]", "op1")
            page.fill("[data-field=password]", "password1")
            page.select_option("[data-field=role]", "viewer")
            page.click("#nu-btn")
            page.wait_for_timeout(800)
            row = page.locator("#page tbody tr").filter(has_text="op1").first
            check("账号：UI 创建 op1", row.count() == 1 and "浏览者" in row.inner_text())

            row.locator("[data-act=status]").click()
            page.wait_for_timeout(700)
            row = page.locator("#page tbody tr").filter(has_text="op1").first
            check("账号：停用", "停用" in row.inner_text())
            row.locator("[data-act=status]").click()
            page.wait_for_timeout(700)
            row = page.locator("#page tbody tr").filter(has_text="op1").first
            check("账号：启用", "启用" in row.inner_text())

            row.locator("[data-act=edit]").click()
            page.wait_for_selector("#ue-role")
            page.select_option("#ue-role", "operator")
            page.locator("[data-act=ue-save]").click()
            page.wait_for_timeout(800)
            row = page.locator("#page tbody tr").filter(has_text="op1").first
            check("账号：行内改角色为操作员", "操作员" in row.inner_text())

            row.locator("[data-act=edit]").click()
            page.wait_for_selector(".ui-row-edit")
            check("账号：行内编辑含设备范围",
                  "设备范围" in page.locator(".ui-row-edit").inner_text()
                  and page.locator("#ue-pass").count() == 1)
            page.locator("[data-act=ue-cancel]").click()
            page.wait_for_timeout(400)

            row.locator("[data-act=del]").click()
            page.wait_for_selector("[data-ok]")
            page.click("[data-ok]")
            page.wait_for_timeout(800)
            check("账号：删除", page.locator("#page tbody tr").filter(has_text="op1").count() == 0)

            # ---------------- 文件：上传 / 复制 / 下载 / 删除 ----------------
            fname = "act-%d.txt" % os.getpid()
            src = os.path.join(work, fname)
            open(src, "w").write("hello actions\n")
            page.goto(base + "/#/files")
            page.wait_for_selector("#upload-input")
            page.set_input_files("#upload-input", src)
            page.click("#upload-btn")
            page.wait_for_selector("#page tbody tr [data-act=download]", timeout=10000)
            page.wait_for_timeout(500)
            frow = page.locator("#page tbody tr").filter(has_text=fname).first
            check("文件：上传", frow.count() == 1)

            frow.locator("[data-act=copy]").click()
            page.wait_for_timeout(500)
            clip = page.evaluate("navigator.clipboard.readText()")
            check("文件：复制哈希", len(clip) == 64, clip[:16])

            with page.expect_download() as dl:
                frow.locator("[data-act=download]").click()
            dst = os.path.join(work, "downloaded.txt")
            dl.value.save_as(dst)
            check("文件：下载内容一致", open(dst).read() == "hello actions\n")

            frow.locator("[data-act=del]").click()
            page.wait_for_selector("[data-ok]")
            page.click("[data-ok]")
            page.wait_for_timeout(800)
            check("文件：删除", page.locator("#page tbody tr").filter(has_text=fname).count() == 0)

            # ---------------- 任务：分发 / 列表 / 详情 ----------------
            dfname = "dist-%d.txt" % os.getpid()
            dsrc = os.path.join(work, dfname)
            open(dsrc, "w").write("hello task\n")
            page.goto(base + "/#/files")
            page.wait_for_selector("#upload-input")
            page.set_input_files("#upload-input", dsrc)
            page.click("#upload-btn")
            page.wait_for_selector("#page tbody tr [data-act=push]", timeout=10000)
            page.wait_for_timeout(400)
            page.locator("#page tbody tr").filter(has_text=dfname).first.locator("[data-act=push]").click()
            page.wait_for_selector("#dist-submit", timeout=8000)
            page.locator("#page .dist-pick").first.check()
            page.wait_for_timeout(300)
            page.click("#dist-submit")
            page.wait_for_timeout(1500)

            page.goto(base + "/#/tasks")
            page.wait_for_selector("#page .detail-table tbody a", timeout=8000)
            head = page.eval_on_selector_all("#page .detail-table thead th", "els => els.map(e => e.textContent.trim())")
            check("任务：列表 10 列", len(head) == 10 and head[-1] == "操作", head)
            check("任务：列表有行", page.eval_on_selector_all("#page .detail-table tbody tr", "els => els.length") >= 1)

            page.locator("#page .detail-table tbody a").first.click()
            page.wait_for_selector(".kv-row", timeout=8000)
            kv = page.eval_on_selector_all(".kv-row", "els => els.map(e => e.innerText.replace(/\\s+/g, ' '))")
            check("任务：详情 8 项", len(kv) == 8 and "设备" in kv[0] and "操作人" in kv[7], kv)
            page.go_back()
            page.wait_for_timeout(600)

            # ---------------- 设备：强制下线 ----------------
            page.goto(base + "/#/agents")
            page.wait_for_selector("#page tbody tr [data-act=kick]", timeout=10000)
            arow = page.locator("#page tbody tr").filter(has_text="dev1").first
            arow.locator("td").nth(1).click()
            page.wait_for_timeout(600)
            check("设备：整行点击进详情", "/#/agents/dev1" in page.url, page.url)
            page.go_back()
            page.wait_for_selector("#page tbody tr [data-act=kick]", timeout=8000)
            arow = page.locator("#page tbody tr").filter(has_text="dev1").first
            arow.locator("[data-act=kick]").click()
            page.wait_for_selector("[data-ok]")
            page.click("[data-ok]")
            page.wait_for_timeout(600)
            toast = page.locator("#toast .item").last.inner_text() if page.locator("#toast .item").count() else ""
            check("设备：强制下线", "已下线 dev1" in toast, toast)

            check("全程无 JS 报错", len(errors) == 0, errors[:3])

        print("=== %d/%d passed, work=%s ===" % (total[0] - len(fails), total[0], work))
        if fails:
            print("FAILED:", ", ".join(fails))
        return 0 if not fails else 1
    finally:
        if browser:
            try:
                browser.close()
            except Exception:
                pass
        for p in reversed(procs):
            try:
                p.terminate()
                p.wait(timeout=3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        log.close()
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
