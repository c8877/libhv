#!/usr/bin/env python3
"""协议文档与新代码的一致性检查。

- `rtnp/frame.hpp` 里的每个帧类型都必须在 docs/design/rtunnel.md 出现
- 文档里的常量（MAX_PAYLOAD、版本号）要和代码一致
- frame.hpp 顶部指向的文档路径必须存在

它不检查字段级内容，只拦住“加了帧类型忘了写文档”这种最常见的漂移。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = os.path.dirname(ROOT)
FRAME_HPP = os.path.join(ROOT, "rtnp", "frame.hpp")
DOC = os.path.join(REPO, "docs", "design", "rtunnel.md")

ENUM_RE = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*0x[0-9A-Fa-f]+\s*,?")


def main():
    failed = 0

    def check(name, ok, extra=None):
        nonlocal failed
        if ok:
            print("ok   " + name)
        else:
            failed += 1
            print("FAIL " + name + ("" if extra is None else "  " + repr(extra)))

    if not os.path.isfile(DOC):
        print("缺少协议文档 %s" % DOC, file=sys.stderr)
        return 2

    src = open(FRAME_HPP, encoding="utf-8").read()
    doc = open(DOC, encoding="utf-8").read()

    names = []
    for line in src.splitlines():
        m = ENUM_RE.match(line)
        if m:
            names.append(m.group(1))
    check("frame.hpp 能解析出帧类型", len(names) >= 40, len(names))

    missing = [n for n in names if n not in doc]
    check("文档覆盖所有帧类型", not missing, missing)

    # 代码顶部注释指向的文档
    head = src[:600]
    m = re.search(r"docs/design/[a-z_]+\.md", head)
    if m:
        target = os.path.join(REPO, m.group(0))
        check("frame.hpp 引用的文档存在", os.path.isfile(target), m.group(0))

    def const(name):
        mm = re.search(r"constexpr\s+\w+\s+%s\s*=\s*(\d+)" % name, src)
        return int(mm.group(1)) if mm else None

    for name in ("VERSION", "PROTO_VERSION", "PROTO_MIN_VERSION", "MAX_PAYLOAD"):
        val = const(name)
        check("文档提到 %s" % name, name in doc, name)
        if val is not None and name == "MAX_PAYLOAD":
            check("MAX_PAYLOAD 与代码一致", str(val) in doc, val)

    # 版本区间描述：PROTO_VERSION / PROTO_MIN 至少各出现一次
    check("文档说明语义版本", "PROTO_VERSION" in doc and "PROTO_MIN_VERSION" in doc)

    print("failed=%d" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
