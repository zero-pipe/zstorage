#!/usr/bin/env python3
"""端到端冒烟测试：起真实服务进程 → RTMP 推流 → HTTP-FLV / HLS 拉流。

为什么需要它：单元测试覆盖不到「协议栈串起来能否工作」。历史上 HLS 卡死、
WebRTC 无音频、G.711 爆音等问题，单测全绿但真实链路是坏的。

设计要点：
  - 只用标准库，不引入第三方依赖（CI 环境干净）
  - 使用独立端口（19xxx），避免与开发机上的实例冲突
  - 自带 RTMP 推流器（够用的最小集），不依赖 ffmpeg
  - 失败时打印服务端日志尾部，便于定位

用法：python3 tests/smoke_e2e.py [--keep]

退出码：0 = 通过；1 = 失败
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

# ── 配置 ────────────────────────────────────────────────────────────────────
RTMP_PORT = 19355
HTTP_PORT = 18085

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_server_binary():
    """在常见构建目录里找 zcs 可执行文件。"""
    names = ["zcs.exe", "zcs"]
    roots = [
        os.path.join(REPO, "build", "examples", "Release"),
        os.path.join(REPO, "build", "examples"),
        os.path.join(REPO, "build"),
    ]
    for root in roots:
        for name in names:
            p = os.path.join(root, name)
            if os.path.isfile(p):
                return p
    return None


def write_config(path):
    """最小配置：只开 RTMP + HTTP，独立端口，关闭 hook 与鉴权。"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            f"""[general]
log_file={os.path.join(os.path.dirname(path), 'smoke.log')}
log_level=warn
poller_threads=2
listen_host=127.0.0.1
externIP=127.0.0.1

[hook]
enable=0

[auth]
enable=0

[rtmp]
port={RTMP_PORT}

[rtsp]
port=18554

[http]
port={HTTP_PORT}

[protocol]
enable_rtmp=1
enable_rtsp=0
enable_srt=0
enable_hls=1
hls_demand=0
enable_audio=1
add_mute_audio=0

[hls]
seg_dur=1
seg_num=3
"""
        )


def wait_port(host, port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


# ── 推流：优先用 ffmpeg（本地与 CI 都常见），无则跳过推流用例 ────────────
def find_ffmpeg():
    for cand in (
        os.environ.get("ZCS_FFMPEG"),
        os.environ.get("FFMPEG"),
        "ffmpeg",
        r"E:8_ffmpeg-latestinfmpeg.exe",
    ):
        if not cand:
            continue
        if os.path.sep in cand or "/" in cand:
            if os.path.isfile(cand):
                return cand
            continue
        for path in os.environ.get("PATH", "").split(os.pathsep):
            p = os.path.join(path, cand)
            if os.path.isfile(p) or os.path.isfile(p + ".exe"):
                return p
    return None


def start_pusher(ffmpeg, app, stream):
    """起一个 ffmpeg 推流进程（合成源，无需素材文件）。"""
    url = f"rtmp://127.0.0.1:{RTMP_PORT}/{app}/{stream}"
    cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-re",
        "-f", "lavfi", "-i", "testsrc=size=320x180:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-g", "25", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "64k", "-ar", "44100", "-ac", "2",
        "-f", "flv", url,
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def _header_value(head, name):
    """从原始响应头里取某个头的值（大小写不敏感）。返回 None 表示没有。"""
    want = name.lower().encode()
    for line in head.split(b"\r\n")[1:]:
        k, sep, v = line.partition(b":")
        if sep and k.strip().lower() == want:
            return v.strip()
    return None


def http_get(path, timeout=5.0, want_bytes=65536):
    """取一个 HTTP 响应。

    返回 (status, body_bytes)。

    两类目标：
      - 定长响应（HLS 清单/API）：按 Content-Length 收齐即返回
      - 直播流（HTTP-FLV）：不会 EOF，收够 want_bytes 或超时就返回
    """
    req = f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    data = b""
    head = b""
    expect_total = None  # 头部结束后，期望的总长度（含头）
    deadline = time.time() + timeout
    with socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=timeout) as s:
        s.sendall(req.encode())
        while time.time() < deadline:
            try:
                s.settimeout(max(0.2, deadline - time.time()))
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk

            if not head and b"\r\n\r\n" in data:
                head, _, _ = data.partition(b"\r\n\r\n")
                cl = _header_value(head, "Content-Length")
                if cl is not None:
                    try:
                        expect_total = len(head) + 4 + int(cl)
                    except ValueError:
                        expect_total = None
                if expect_total is None:
                    # 无 Content-Length（流式）：收够 want_bytes 即可
                    if len(data) >= want_bytes:
                        break
            if expect_total is not None and len(data) >= expect_total:
                break
    head, _, body = data.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0].decode(errors="replace")
    try:
        status = int(status_line.split()[1])
    except (IndexError, ValueError):
        status = 0
    return status, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="测试结束保留临时目录")
    args = ap.parse_args()

    binary = find_server_binary()
    if not binary:
        print("FAIL: 找不到 zcs，请先构建 examples")
        return 1

    workdir = tempfile.mkdtemp(prefix="zcs_smoke_")
    cfg = os.path.join(workdir, "smoke.ini")
    write_config(cfg)

    print(f"[smoke] server = {binary}")
    print(f"[smoke] workdir = {workdir}")

    proc = subprocess.Popen(
        [binary, "--config", cfg],
        cwd=workdir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    log_lines = []

    def drain():
        for line in proc.stdout:
            log_lines.append(line.decode(errors="replace").rstrip())

    threading.Thread(target=drain, daemon=True).start()

    failures = []

    def fail(msg):
        print(f"FAIL: {msg}")
        failures.append(msg)

    try:
        if not wait_port("127.0.0.1", RTMP_PORT):
            fail("RTMP 端口未就绪")
        if not wait_port("127.0.0.1", HTTP_PORT):
            fail("HTTP 端口未就绪")

        if not failures:
            ffmpeg = find_ffmpeg()
            pusher = None

            if not ffmpeg:
                print("[smoke] SKIP 推流用例：未找到 ffmpeg（设 ZCS_FFMPEG 可指定路径）")
            else:
                print(f"[smoke] ffmpeg = {ffmpeg}")
                pusher = start_pusher(ffmpeg, "live", "smoke")
                # 等流真正建立：轮询 FLV 端点而不是盲等
                online = False
                for _ in range(40):  # 最多 10s
                    time.sleep(0.25)
                    if pusher.poll() is not None:
                        break
                    try:
                        st, _b = http_get("/live/smoke.live.flv", timeout=1.5, want_bytes=16)
                    except OSError:
                        continue
                    if st == 200:
                        online = True
                        break
                if pusher.poll() is not None:
                    err = pusher.stderr.read().decode(errors="replace")[-600:]
                    fail(f"ffmpeg 推流进程提前退出：{err}")
                elif not online:
                    fail("推流后 HTTP-FLV 始终未返回 200")
                else:
                    print("[smoke] RTMP 推流 + HTTP-FLV 建流 OK")

                if online:
                    # 1) FLV 应持续出数据
                    try:
                        status, body = http_get("/live/smoke.live.flv", timeout=4.0,
                                                want_bytes=4000)
                        if status != 200:
                            fail(f"HTTP-FLV 状态码 {status}（期望 200）")
                        elif len(body) < 1000:
                            fail(f"HTTP-FLV 数据过少：{len(body)} 字节")
                        else:
                            print(f"[smoke] HTTP-FLV OK ({len(body)} bytes)")
                    except OSError as exc:
                        fail(f"HTTP-FLV 请求失败：{exc}")

                    # 2) HLS 清单
                    try:
                        status, body = http_get("/live/smoke.m3u8", timeout=4.0, want_bytes=64)
                        if status != 200:
                            fail(f"HLS m3u8 状态码 {status}（期望 200）")
                        elif b"#EXTM3U" not in body:
                            fail("HLS m3u8 内容不含 #EXTM3U")
                        else:
                            print(f"[smoke] HLS m3u8 OK ({len(body)} bytes)")
                    except OSError as exc:
                        fail(f"HLS 请求失败：{exc}")

                    # 3) WebAPI 存活（本机 secret 为空时允许访问）
                    try:
                        status, body = http_get("/index/api/system/config", timeout=4.0,
                                                want_bytes=64)
                        if status == 200 and b"code" in body:
                            print("[smoke] WebAPI OK")
                        else:
                            fail(f"WebAPI 异常：status={status}")
                    except OSError as exc:
                        fail(f"WebAPI 请求失败：{exc}")

            if pusher and pusher.poll() is None:
                pusher.terminate()
                try:
                    pusher.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pusher.kill()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if failures:
        print("\n── 服务端日志尾部 ──")
        for line in log_lines[-40:]:
            print(line)
        if not args.keep:
            pass
        return 1

    print("smoke_e2e: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
