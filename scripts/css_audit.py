#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""管理台 CSS 体检：重复选择器、未引用类、字面量颜色、!important。

用法:
    python3 scripts/css_audit.py [--root DIR] [--strict]

默认只报告；--strict 时存在重复选择器返回非零，可用于提交前检查。
"""

import argparse
import os
import re
import sys

# 由 JS 动态拼接的类名，不参与未引用判定
DYNAMIC_PREFIX = ("cols-",)


def parse_rules(src):
    """返回 [(selector, start_line)]，只取顶层规则（@media 内部单独统计）。"""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    lines = src.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if re.match(r"^[.#\[a-zA-Z*]", line) and "{" in line and not line.startswith("@"):
            sel = line.split("{")[0].strip()
            out.append((sel, i + 1))
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "rtunnel", "relay", "admin", "frontend"))
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    css_path = os.path.join(args.root, "admin.css")
    if not os.path.exists(css_path):
        sys.exit("找不到 " + css_path)
    css = open(css_path).read()
    rules = parse_rules(css)

    seen = {}
    for sel, ln in rules:
        key = ",".join(x.strip() for x in sel.split(",") if x.strip())
        seen.setdefault(key, []).append(ln)
    dups = {s: v for s, v in seen.items() if len(v) > 1}

    body = re.sub(r"\{[^{}]*\}", "{}", css)
    classes = set(re.findall(r"\.([A-Za-z][\w-]*)", body))
    sources = ""
    for name in os.listdir(args.root):
        if name.endswith((".js", ".html")):
            sources += open(os.path.join(args.root, name), errors="ignore").read()
    unused = sorted(c for c in classes
                    if c not in sources and not c.startswith(DYNAMIC_PREFIX))

    important = css.count("!important")
    literals = re.findall(r"#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)", css)
    # var(--x, #fallback) 里的回退值不算硬编码
    fallbacks = re.findall(r"var\([^)]*,\s*(#[0-9a-fA-F]{3,8}|rgba?\([^)]*\))", css)
    hard_colors = len(literals) - len(fallbacks)

    print("文件      %s" % css_path)
    print("规模      %d 行 / %d 字节 / %d 条规则 / %d 个类选择器"
          % (len(css.split("\n")), len(css), len(rules), len(classes)))
    print("重复定义  %d" % len(dups))
    for s, lines in sorted(dups.items(), key=lambda kv: kv[1][0]):
        print("          %-40s 行 %s" % (s, lines))
    print("未引用类  %d%s" % (len(unused), ("  " + " ".join(unused)) if unused else ""))
    print("!important %d" % important)
    print("字面量颜色 %d（不含 var 回退值）" % hard_colors)

    bad = bool(dups)
    if args.strict and bad:
        print("\n存在重复定义的选择器，请合并到原规则")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
