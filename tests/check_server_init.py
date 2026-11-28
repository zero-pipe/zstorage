#!/usr/bin/env python3
"""检查服务启动时必须完成的全局初始化。

背景：HLS 裁剪时误删了 zcs_http_hls_init()，它顺带调用
zcs_media_events_set() 注册 poller；删掉后录像与回放都拿不到 poller。
这里把这类"藏在被裁剪模块里的副作用"固定为显式检查。

用法：python3 check_server_init.py <zcs_server.c>
退出码：0 = 通过；1 = 缺少必需的初始化调用。
"""
import re
import sys

# server_start_services() 中必须直接出现的初始化调用。
REQUIRED_CALLS = (
    ("zcs_media_events_set(", "全局 events poller（录像/回放依赖）"),
    ("zcs_media_events_set_continue_push_ms(", "断流保留时长"),
    ("zcs_auth_init(", "鉴权配置"),
    ("zcs_webhook_init(", "平台回调"),
)


def main():
    if len(sys.argv) != 2:
        print("usage: check_server_init.py <zcs_server.c>", file=sys.stderr)
        return 1
    try:
        text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    except OSError as exc:
        print(f"无法读取 {sys.argv[1]}: {exc}", file=sys.stderr)
        return 1

    m = re.search(r"static\s+zn_err_t\s+server_start_services\s*\([^)]*\)\s*\{", text)
    if not m:
        print("FAIL: 找不到 server_start_services() 定义", file=sys.stderr)
        return 1
    # 简单括号配平提取函数体，足够覆盖本项目代码风格。
    start = m.end() - 1
    depth = 0
    end = start
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = text[start:end]

    bad = 0
    for call, desc in REQUIRED_CALLS:
        if call not in body:
            print(f"FAIL: server_start_services() 缺少 {call}  # {desc}", file=sys.stderr)
            bad += 1
    if bad:
        return 1
    print("OK: server_start_services() 全局初始化完整")
    return 0


if __name__ == "__main__":
    sys.exit(main())
