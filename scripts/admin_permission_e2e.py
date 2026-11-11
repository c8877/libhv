#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""管理台权限功能端到端用例（Playwright，10 条）。

自启 relay + 3 个 agent，初始化管理员后全部通过管理台 UI 操作：
建分组、配能力、加 ACL、有效权限、字典、旧 ACL 导入导出、删除。

用法:
    python3 scripts/admin_permission_e2e.py [--bin DIR] [--keep] [--slow]

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


class Runner:
    def __init__(self, page, work, slow=False):
        self.page = page
        self.work = work
        self.slow = slow
        self.fails = []
        self.total = 0

    def check(self, name, ok, detail=""):
        self.total += 1
        print(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail) + "]") if detail and not ok else ""))
        if not ok:
            self.fails.append(name)
            self.shot(name)
        return ok

    def shot(self, name, fail=True):
        path = os.path.join(self.work, "shots", ("fail-" if fail else "pass-") + "".join(
            c if c.isalnum() or c in "-_" else "_" for c in name) + ".png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        try:
            self.page.screenshot(path=path, full_page=True)
            print("     screenshot:", path)
        except Exception:
            pass

    def goto_permission(self):
        self.page.goto(self.base + "/#/permission")
        self.page.wait_for_selector("#perm-tabs .ui-tab", timeout=8000)
        self.page.wait_for_timeout(300)

    def tab(self, key):
        self.page.click('#perm-tabs .ui-tab[data-key="%s"]' % key)
        self.page.wait_for_timeout(450)

    def toast(self, timeout=6000):
        try:
            self.page.wait_for_selector("#toast .item", timeout=timeout)
            return self.page.locator("#toast .item").last.inner_text()
        except Exception:
            return ""

    def close_modal(self):
        self.page.locator(".tty-mask").last.locator("[data-modal-close]").first.click()
        self.page.wait_for_timeout(250)

    def row(self, text):
        return self.page.locator(".perm-row").filter(has_text=text).first

    def expand(self, text):
        self.row(text).click()
        self.page.wait_for_timeout(600)

    def dict_row(self, value):
        rows = self.page.locator("#dict-body tbody tr")
        for i in range(rows.count()):
            code = rows.nth(i).locator("code")
            if code.count() and code.inner_text().strip() == value:
                return rows.nth(i)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "build", "bin"))
    ap.add_argument("--keep", action="store_true", help="保留临时数据库与截图")
    ap.add_argument("--slow", action="store_true", help="放慢操作，便于观察")
    args = ap.parse_args()

    relay_bin = os.path.join(args.bin, "rtunnel-relay")
    agent_bin = os.path.join(args.bin, "rtunnel-agent")
    for p in (relay_bin, agent_bin):
        if not os.path.exists(p):
            sys.exit("找不到二进制: " + p)

    work = tempfile.mkdtemp(prefix="perm-ui-")
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

    for devid, allow in (("dev1", "22,80"), ("dev2", "22,80"), ("dev3", "22")):
        procs.append(subprocess.Popen(
            [agent_bin, "--server", "127.0.0.1", "--port", str(relay_port),
             "--devid", devid, "--allow", allow,
             "--config-file", os.path.join(work, "agent-%s.json" % devid)],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, start_new_session=True))

    browser = None
    ok = False
    try:
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            page = browser.new_page(viewport={"width": 1440, "height": 950})
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            r = Runner(page, work, args.slow)
            r.base = base

            # ---------------- 用例 1：初始化并登录，权限页初始状态 ----------------
            page.goto(base, wait_until="networkidle")
            page.fill("#af-token", "boot")
            page.fill("#af-user", "root")
            page.fill("#af-pass", "rootpass1")
            page.click("#af-btn")
            page.wait_for_timeout(1200)
            r.goto_permission()
            tabs = page.eval_on_selector_all(
                "#perm-tabs .ui-tab", "els=>els.map(e=>e.innerText.replace(/\\n/g,' '))")
            empty = page.locator(".perm-table .empty").count() > 0
            btns = page.eval_on_selector_all(
                ".panel-head button", "els=>els.map(e=>e.textContent.trim())")
            r.check("1 登录并进入权限页", len(tabs) == 2 and empty
                    and "有效权限" in btns and "字典管理" in btns, (tabs, empty, btns))

            # ---------------- 用例 2：设备分组 + 穿梭框添加设备 ----------------
            r.tab("agent")
            page.click("#perm-new")
            page.wait_for_selector("#pa-name")
            page.fill("#pa-name", "产线设备")
            page.fill("#pa-desc", "产线相关设备")
            page.click("#pa-transfer")
            page.wait_for_selector("#tp-left .transfer-item", timeout=8000)
            page.wait_for_function(
                "document.querySelectorAll('#tp-left .transfer-item').length >= 3", timeout=8000)
            page.click('#tp-left input.tp-pick[value="dev1"]')
            page.click('#tp-left input.tp-pick[value="dev2"]')
            page.click("#tp-add")
            right = page.eval_on_selector_all(
                "#tp-right .transfer-dev", "els=>els.map(e=>e.textContent)")
            page.click("#tp-ok")
            page.wait_for_timeout(400)
            chips = page.eval_on_selector_all(
                "#pa-devices .perm-chip", "els=>els.map(e=>e.dataset.v)")
            page.click("#pa-save")
            toast = r.toast()
            r.check("2 设备分组用穿梭框添加 dev1/dev2", sorted(right) == ["dev1", "dev2"]
                    and sorted(chips) == ["dev1", "dev2"] and "已保存" in toast, (right, chips, toast))
            page.wait_for_timeout(400)

            # ---------------- 用例 3：设备分组能力上限 ----------------
            if page.locator("#pa-ports-edit").count() == 0:
                r.expand("产线设备")
            page.click("#pa-ports-edit")
            page.wait_for_selector("#tp-addon")
            for val, name in (("22", "SSH"), ("80", "HTTP")):
                page.click("#tp-addon")
                page.wait_for_selector("#dict-val")
                page.fill("#dict-val", val)
                page.fill("#dict-name", name)
                page.click("#dict-ok")
                page.wait_for_function(
                    "() => [...document.querySelectorAll('#tp-right .transfer-dev')]"
                    ".some(e => e.textContent === %r)" % val, timeout=5000)
            page.click("#tp-ok")
            page.wait_for_timeout(350)
            ports_view = page.locator("#pa-ports-view").inner_text()
            page.check("#pa-shell")
            page.select_option("#pa-file", "rw")
            page.click("#pa-save")
            toast = r.toast()
            caps = page.eval_on_selector("#pa-ports", "e=>e.value")
            r.check("3 设备分组设置端口 22/80、Shell、文件 rw",
                    "22" in ports_view and "80" in ports_view
                    and caps == "22,80" and "已保存" in toast, (ports_view, caps, toast))
            page.wait_for_timeout(400)

            # ---------------- 用例 4：客户端分组 + 用户 + ACL ----------------
            r.tab("client")
            page.click("#perm-new")
            page.wait_for_selector("#pc-name")
            page.fill("#pc-name", "运维组")
            page.fill("#pc-desc", "运维人员")
            page.fill("#pc-user", "alice,bob")
            page.click("#pc-add-user")
            page.wait_for_timeout(250)
            n1 = page.locator("#pc-users .perm-chip").count()
            page.locator("#pc-users .perm-chip").nth(1).locator(".perm-chip-x").click()
            page.wait_for_timeout(200)
            n2 = page.locator("#pc-users .perm-chip").count()
            page.fill("#pc-user", "bob")
            page.click("#pc-add-user")
            page.wait_for_timeout(200)
            n3 = page.locator("#pc-users .perm-chip").count()
            users = page.eval_on_selector_all(
                "#pc-users .perm-chip", "els=>els.map(e=>e.dataset.v)")
            page.click("#pc-add-acl")
            page.wait_for_selector("#pc-acl .perm-acl-row")
            page.locator("#pc-acl .perm-acl-row").first.locator(".pa-ports-edit").click()
            page.wait_for_selector("#tp-left .transfer-item", timeout=5000)
            for val in ("22", "80"):
                page.click('#tp-left input.tp-pick[value="%s"]' % val)
            page.click("#tp-add")
            page.click("#tp-ok")
            page.wait_for_timeout(350)
            page.locator("#pc-acl .pa-shell").first.check()
            page.locator("#pc-acl .pa-file").first.select_option("ro")
            page.click("#pc-save")
            toast = r.toast()
            rowtext = r.row("运维组").inner_text().replace("\n", " | ")
            r.check("4 客户端分组：用户增删 + ACL 关联设备组",
                    n1 == 2 and n2 == 1 and n3 == 2 and sorted(users) == ["alice", "bob"]
                    and "已保存" in toast and "2" in rowtext, (n1, n2, n3, users, rowtext))
            page.wait_for_timeout(400)

            # ---------------- 用例 5：刷新后回读客户端分组 ----------------
            page.reload(wait_until="networkidle")
            page.wait_for_selector("#perm-tabs .ui-tab", timeout=8000)
            page.wait_for_timeout(500)
            r.expand("运维组")
            page.wait_for_selector("#pc-name")
            users2 = page.eval_on_selector_all(
                "#pc-users .perm-chip", "els=>els.map(e=>e.dataset.v)")
            acl_ports = page.eval_on_selector("#pc-acl .pa-ports", "e=>e.value")
            acl_shell = page.eval_on_selector("#pc-acl .pa-shell", "e=>e.checked")
            acl_file = page.eval_on_selector("#pc-acl .pa-file", "e=>e.value")
            acl_n = page.locator("#pc-acl .perm-acl-row").count()
            r.check("5 刷新后分组数据完整回读",
                    sorted(users2) == ["alice", "bob"] and acl_n == 1
                    and acl_ports == "22,80" and acl_shell and acl_file == "ro",
                    (users2, acl_n, acl_ports, acl_shell, acl_file))

            # ---------------- 用例 6：有效权限预览 ----------------
            page.click("#perm-effective")
            page.wait_for_selector("#pe-client")
            page.fill("#pe-client", "alice")
            page.click("#pe-check")
            page.wait_for_function(
                "() => document.querySelector('#pe-body') && "
                "document.querySelector('#pe-body').innerText.includes('有效能力')", timeout=6000)
            body = page.locator("#pe-body").inner_text().replace("\n", " | ")
            r.check("6 有效权限按上限求交",
                    "运维组" in body and "dev1" in body and "dev2" in body
                    and "TCP 22,80" in body and "Shell" in body
                    and "文件 ro" in body, body)
            r.shot("有效权限预览", fail=False)
            r.close_modal()

            # ---------------- 用例 7：字典批量新增 ----------------
            page.click("#perm-dict")
            page.wait_for_selector("#dict-body")
            page.click("#dict-batch")
            page.wait_for_selector("#db-values")
            page.fill("#db-values", "8080,9000-9010")
            page.wait_for_timeout(200)
            page.click("#db-ok")
            page.wait_for_timeout(900)
            values = page.eval_on_selector_all(
                "#dict-body code", "els=>els.map(e=>e.textContent)")
            r.check("7 字典批量新增端口", "8080" in values and "9000-9010" in values, values)

            # ---------------- 用例 8：停用端口即时生效 ----------------
            row80 = r.dict_row("80")
            row80.locator(".dict-edit").click()
            page.wait_for_selector("#de-enabled")
            page.click("#de-enabled .ui-switch-track")
            page.click("[data-act=dict-save]")
            page.wait_for_timeout(900)
            status80 = r.dict_row("80").inner_text()
            r.close_modal()
            page.click("#perm-effective")
            page.wait_for_selector("#pe-client")
            page.fill("#pe-client", "alice")
            page.click("#pe-check")
            page.wait_for_function(
                "() => document.querySelector('#pe-body') && "
                "document.querySelector('#pe-body').innerText.includes('有效能力')", timeout=6000)
            body = page.locator("#pe-body").inner_text()
            r.check("8 字典停用后有效权限剔除 80",
                    "停用" in status80 and "TCP 22" in body and "TCP 80" not in body, body[:200])
            r.close_modal()
            page.click("#perm-dict")
            page.wait_for_selector("#dict-body")
            r.dict_row("80").locator(".dict-edit").click()
            page.wait_for_selector("#de-enabled")
            page.click("#de-enabled .ui-switch-track")
            page.click("[data-act=dict-save]")
            page.wait_for_timeout(800)
            r.check("8b 重新启用 80", "启用" in r.dict_row("80").inner_text())

            # ---------------- 用例 9：被引用字典项删除保护 ----------------
            row22 = r.dict_row("22")
            row22.locator(".dict-del").click()
            page.wait_for_selector("[data-ok]")
            confirm_text = page.locator(".tty-mask").last.inner_text()
            page.click("[data-ok]")
            page.wait_for_timeout(900)
            status22 = r.dict_row("22").inner_text()
            r.check("9 被引用项删除被拒并转为停用",
                    "无法删除" in confirm_text and "引用" in confirm_text
                    and "停用" in status22, (confirm_text[:80], status22))
            r.dict_row("22").locator(".dict-edit").click()
            page.wait_for_selector("#de-enabled")
            page.click("#de-enabled .ui-switch-track")
            page.click("[data-act=dict-save]")
            page.wait_for_timeout(800)
            r.check("9b 恢复 22 启用", "启用" in r.dict_row("22").inner_text())
            r.close_modal()

            # ---------------- 用例 10：旧 ACL 导入 / 导出 / 删除 ----------------
            page.click("#perm-legacy")
            page.wait_for_selector("#ia-content")
            page.fill("#ia-content", "carol=dev3")
            page.click("#ia-ok")
            page.wait_for_function(
                "() => (document.querySelector('#ia-result')||{}).textContent"
                ".includes('已导入 1')", timeout=8000)
            page.wait_for_timeout(1200)
            has_carol = page.locator(".perm-row").filter(has_text="carol").count() > 0
            page.click("#perm-legacy")
            page.wait_for_selector("#legacy-tabs .ui-tab[data-key=export]")
            page.click("#legacy-tabs .ui-tab[data-key=export]")
            page.wait_for_timeout(250)
            page.click("#ea-run")
            page.wait_for_function(
                "() => (document.querySelector('#ea-content')||{}).value.length > 0", timeout=8000)
            exported = page.eval_on_selector("#ea-content", "e=>e.value")
            r.check("10 旧 ACL 导入并导出 carol=dev3",
                    has_carol and "carol=dev3" in exported, (has_carol, exported[:300]))
            r.close_modal()
            page.wait_for_timeout(300)
            r.row("carol").locator(".perm-del").click()
            page.wait_for_selector("[data-ok]")
            page.click("[data-ok]")
            page.wait_for_timeout(900)
            gone = page.locator(".perm-row").filter(has_text="carol").count() == 0
            page.click("#perm-effective")
            page.wait_for_selector("#pe-client")
            page.fill("#pe-client", "carol")
            page.click("#pe-check")
            page.wait_for_timeout(700)
            eff = page.locator("#pe-body").inner_text()
            r.check("10b 删除分组后 carol 无分组", gone and "未加入客户端分组" in eff, (gone, eff[:120]))
            r.close_modal()

            r.check("附：全程无 JS 报错", len(errors) == 0, errors[:3])

        ok = len(r.fails) == 0
        print("=== %d/%d passed, work=%s ===" % (r.total - len(r.fails), r.total, work))
        if r.fails:
            print("FAILED:", ", ".join(r.fails))
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

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
