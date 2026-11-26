#!/usr/bin/env python3
"""检查 zn_err_t 函数的返回值风格。

规则（见 docs/stream-pipeline.md 第 7.1 节）：
  声明为 zn_err_t 的函数不得 `return -1`——zn_err_t 里 -1 是 ZN_ERR_NOMEM，
  裸 -1 会让调用方误判为内存不足。

用法：python3 tools/check_errno_style.py [路径...]
      无参数时扫描 src/
退出码：0 = 通过；1 = 有违规。
"""
import os
import re
import sys

FUNC_RE = re.compile(r'^(static\s+)?zn_err_t\s+\**(\w+)\s*\(')
RET_RE = re.compile(r'^\s*return\s+-1\s*;')


def scan_file(path):
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as exc:
        print(f"warning: 无法读取 {path}: {exc}", file=sys.stderr)
        return []
    bad = []
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(2)
        start = i + 1
        depth = 0
        began = False
        j = i
        while j < len(lines):
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                began = True
            if began and depth == 0:
                break
            j += 1
        for k in range(i, min(j + 1, len(lines))):
            if RET_RE.match(lines[k]):
                bad.append((path, start + k, name))
        i = j + 1
    return bad


def main(argv):
    targets = argv[1:] or ["src"]
    files = []
    for t in targets:
        if os.path.isdir(t):
            for root, _, names in os.walk(t):
                files.extend(os.path.join(root, n) for n in names if n.endswith(".c"))
        elif t.endswith(".c"):
            files.append(t)
    bad = []
    for f in files:
        bad.extend(scan_file(f))
    if bad:
        for path, line, func in bad:
            print(f"{path}:{line}: 函数 {func}() 声明为 zn_err_t，不得 return -1"
                  f"（-1 == ZN_ERR_NOMEM）；改用 ZN_ERR_INVALID/IO/STATE 等具体码")
        return 1
    print(f"check_errno_style: OK ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
