#!/usr/bin/env python3
"""管理台冒烟测试。

前置：relay 已启动；本机安装 playwright：
    pip install playwright && playwright install chromium

用法：
    python3 scripts/admin_smoke.py --base http://127.0.0.1:9443 --user root --pass rootpass1

也可用环境变量：SMOKE_BASE / SMOKE_USER / SMOKE_PASS。

检查项：登录、各页面无 JS 报错、权限页 Tab、旧 ACL 弹窗、主题切换。
"""

import argparse
import os
import sys

PAGES = [
    ("overview", "在线设备"), ("agents", "设备（C 侧）"), ("permission", "权限管理"),
    ("clients", "客户端（A 侧）"), ("streams", "会话"), ("certs", "签发新证书"),
    ("accounts", "账号列表"), ("files", "上传文件"), ("tasks", "任务"),
    ("audit", "审计日志"), ("settings", "设置"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.environ.get("SMOKE_BASE", "http://127.0.0.1:9443"))
    ap.add_argument("--user", default=os.environ.get("SMOKE_USER", ""))
    ap.add_argument("--pass", dest="password", default=os.environ.get("SMOKE_PASS", ""))
    ap.add_argument("--headed", action="store_true", help="显示浏览器窗口")
    args = ap.parse_args()
    if not args.user or not args.password:
        print("需要 --user/--pass 或 SMOKE_USER/SMOKE_PASS")
        return 2
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("未安装 playwright：pip install playwright && playwright install chromium")
        return 2

    fails = []
    stats = {"total": 0}

    def check(name, ok, detail=""):
        stats["total"] += 1
        print(("PASS " if ok else "FAIL ") + name + ((" (" + detail + ")") if detail and not ok else ""))
        if not ok:
            fails.append(name)

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=not args.headed)
        page = browser.new_page(viewport={"width": 1400, "height": 900})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))

        page.goto(args.base, wait_until="networkidle")
        if page.locator("#af-token").count():
            print("FAIL 首次初始化：请先创建管理员再运行")
            browser.close()
            return 1
        page.fill("#af-user", args.user)
        page.fill("#af-pass", args.password)
        page.click("#af-btn")
        page.wait_for_timeout(800)
        check("登录", page.locator("#nav a").count() > 0)

        for name, title in PAGES:
            page.goto(args.base + "/#/" + name)
            page.wait_for_timeout(500)
            text = page.evaluate("document.querySelector('#page')?.innerText || ''")
            check("页面 " + name, title in text, text[:40].replace("\n", " "))

        # 权限页 Tab 切换
        page.goto(args.base + "/#/permission")
        page.wait_for_timeout(600)
        tabs = page.locator("#perm-tabs .ui-tab")
        if tabs.count() >= 2:
            tabs.nth(1).click()
            page.wait_for_timeout(400)
            check("权限 Tab 切换", "设备分组" in page.evaluate("document.querySelector('#perm-tabs')?.innerText || ''"))
            tabs.nth(0).click()
            page.wait_for_timeout(300)
        else:
            check("权限 Tab 切换", False, "tabs missing")

        # 旧 ACL 弹窗
        if page.locator("#perm-legacy").count():
            page.locator("#perm-legacy").click()
            page.wait_for_timeout(500)
            check("旧 ACL 弹窗", page.locator("#legacy-tabs .ui-tab").count() >= 1)
            page.locator(".tty-modal [data-modal-close]").first.click()
            page.wait_for_timeout(200)
        else:
            check("旧 ACL 弹窗", False, "button missing")

        # 主题切换
        page.locator("#themebtn").click()
        page.wait_for_timeout(200)
        page.locator("#thememenu [data-mode=dark]").click()
        page.wait_for_timeout(400)
        check("主题切换", page.evaluate("document.documentElement.getAttribute('data-theme')") == "dark")
        if not page.locator("#thememenu").is_visible():
            page.locator("#themebtn").click()
            page.wait_for_timeout(200)
        page.locator("#thememenu [data-mode=light]").click()
        page.wait_for_timeout(300)

        check("无 JS 报错", not errors, "; ".join(errors[:3]))
        browser.close()

    print("=== %d/%d passed ===" % (stats["total"] - len(fails), stats["total"]))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
