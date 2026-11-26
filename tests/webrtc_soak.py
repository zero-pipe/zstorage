#!/usr/bin/env python3
"""WHEP soak：并发 viewer 拉流，统计帧率/断流/重连。

用法:
    python3 tests/tools/webrtc_soak.py \
        --url http://127.0.0.1:8080/index/api/whep?app=live&stream=cam1 \
        --viewers 8 --seconds 600

依赖: aiortc (pip install aiortc)。每 30s 打印一次汇总；结束时输出每条流帧数。
"""
import argparse
import asyncio
import time
import urllib.request

from aiortc import RTCPeerConnection, RTCSessionDescription


async def post_sdp(url, sdp):
    loop = asyncio.get_event_loop()

    def do():
        req = urllib.request.Request(
            url, data=sdp.encode(), headers={"Content-Type": "application/sdp"}
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read().decode()

    return await loop.run_in_executor(None, do)


async def run_viewer(idx, args, stats):
    """一条 viewer：建立 WHEP，持续收帧；异常后重连直到时间结束。"""
    deadline = time.time() + args.seconds
    stats[idx] = {"frames": 0, "reconnects": 0, "errors": 0}
    while time.time() < deadline:
        pc = RTCPeerConnection()
        done = asyncio.Event()

        @pc.on("track")
        def on_track(track):
            async def recv():
                while True:
                    try:
                        frame = await asyncio.wait_for(track.recv(), 5)
                    except Exception:
                        done.set()
                        return
                    if getattr(frame, "width", 0):
                        stats[idx]["frames"] += 1

            asyncio.ensure_future(recv())

        try:
            pc.addTransceiver("video", direction="recvonly")
            offer = await pc.createOffer()
            await pc.setLocalDescription(offer)
            for _ in range(100):
                if pc.iceGatheringState == "complete":
                    break
                await asyncio.sleep(0.05)
            answer = await post_sdp(args.url, pc.localDescription.sdp)
            await pc.setRemoteDescription(
                RTCSessionDescription(sdp=answer, type="answer")
            )
            try:
                await asyncio.wait_for(done.wait(), max(1, deadline - time.time()))
            except asyncio.TimeoutError:
                pass  # 到点正常结束
        except Exception:
            stats[idx]["errors"] += 1
        finally:
            try:
                await pc.close()
            except Exception:
                pass
        if time.time() < deadline:
            stats[idx]["reconnects"] += 1
            await asyncio.sleep(args.retry)


async def reporter(args, stats, stop):
    while not stop.is_set():
        await asyncio.sleep(30)
        total = sum(s["frames"] for s in stats.values())
        errs = sum(s["errors"] for s in stats.values())
        rec = sum(s["reconnects"] for s in stats.values())
        print(f"[soak] frames={total} errors={errs} reconnects={rec}", flush=True)
        if stop.is_set():
            return


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--url",
        default="http://127.0.0.1:8080/index/api/whep?app=live&stream=cam1",
    )
    ap.add_argument("--viewers", type=int, default=4)
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--retry", type=float, default=1.0)
    args = ap.parse_args()

    stats = {}
    stop = asyncio.Event()
    tasks = [
        asyncio.ensure_future(run_viewer(i, args, stats)) for i in range(args.viewers)
    ]
    rep = asyncio.ensure_future(reporter(args, stats, stop))
    await asyncio.gather(*tasks)
    stop.set()
    await asyncio.sleep(0)
    rep.cancel()
    for i in range(args.viewers):
        s = stats[i]
        print(
            f"[soak] viewer={i} frames={s['frames']} errors={s['errors']} "
            f"reconnects={s['reconnects']}"
        )


if __name__ == "__main__":
    asyncio.run(main())
