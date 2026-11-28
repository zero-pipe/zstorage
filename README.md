# Zero Media Server

ZCS 是我用 C99 写的流媒体服务，也可以当库嵌进自己的进程里用。一路推上来的流，可以按需要用 RTMP、RTSP、HTTP-FLV、HLS、DASH 去播；SRT 和 WebRTC（WHIP/WHEP）默认不开，要用再编进去。

重点不在「协议越多越好」，而在中间那条共用的媒体路径：进来先把时间线和帧形态理顺，放进 GOP 环形缓存，出去再按各协议重封。协议层尽量薄。我更希望你能直接链 `zcs`，自己管启动和退出，而不是只能起一个独立进程当黑盒。

| 依赖 | 作用 | 路径 |
|------|------|------|
| [znet](https://github.com/zero-pipe/zero-tool-kit) | 网络、Poller、缓冲、日志 | `3rdpart/znet` |
| [zmedia](https://github.com/zero-pipe/zero-media-kit) | FLV / RTP / TS / MP4 / HLS / DASH 等 | `3rdpart/zmedia` |

---

## 我在解决什么

做流媒体服务，真正容易出问题的往往不是少一种协议，而是：

- 一路推、多路播时，时间戳和关键帧各搞各的
- 直播缓存、点播、拉流代理、切片互相抢状态
- 想嵌进自己的程序时，只能再拉一个进程，生命周期对不上

所以 ZCS 的选择是：协议可以多，**媒体路径尽量少**。直播进 `gop_queue`，播放和 HLS/DASH 旁路都从同一条环上读；配置和 hooks 走稳定接口，协议内部细节我不保证 ABI 长期不变。

---

## 支持什么

| | |
|--|--|
| 推流 | RTMP、RTSP RECORD、SRT（MPEG-TS）、WHIP |
| 播放 | RTMP、RTSP、HTTP-FLV、HLS、DASH、SRT、WHEP |
| 点播 | 本地文件（例如 `www/vod/`），HTTP / RTSP / RTMP 等按配置走 |
| 代理 | 按需拉流（`[proxy]` 或 `addStreamProxy`） |
| 运维 | HTTP API、可选 webhook、健康检查和缓冲池统计 |
| 嵌入 | `#include "zcs/sdk.h"`，create → start → run → stop → destroy |

编解码组合在 `include/zcs/core/codec_filter.h` 里卡死；入口不支持的直接拒掉，避免半通不通的路径。

默认只编 RTMP / RTSP / HTTP。**SRT、WebRTC 默认关着**，机器上有 libsrt、OpenSSL 再打开。

---

## 快速开始

```bash
git clone https://github.com/zero-pipe/zero-media-server-new.git
cd zero-media-server-new
```

### 最小编译（只要 RTMP / RTSP / HTTP）

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Windows（PowerShell）
cmake -S . -B build -DZN_ENABLE_WEPOLL=ON
cmake --build build --config Release
```

编出来的程序：

- Windows：`build\examples\Release\demo_media_server.exe`
- Linux / macOS：`build/examples/demo_media_server`

### 运行

```bash
cp conf/config.ini config.ini
# 端口按需改；本地的 config.ini 已在 gitignore 里

./build/examples/demo_media_server --config config.ini
# Windows: build\examples\Release\demo_media_server.exe --config config.ini
```

模板里默认端口：RTMP `1935`，RTSP `8554`，HTTP `8080`，SRT `9000`。WebRTC 信令走 HTTP 端口。

```bash
ffmpeg -re -stream_loop -1 -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test
ffplay http://127.0.0.1:8080/live/test.flv
```

起来之后会在终端打出本机推流、播放地址。点播文件放到 `www/vod/`（或配置里 `[record] root` 指的目录）。

---

## 嵌进自己的程序

稳定对外入口就一个：`zcs/sdk.h`（里面会带上 `server.h`）。头文件树与 `src/`
同构（`include/zcs/<layer>/...`），`cmake --install` 会安装 `include/zcs` 下的
全部头；稳定与否由 `ZCS_API` 标注——`sdk.h` / `server.h` 及其直接依赖是承诺
稳定的入口，其余头可用但不保证 ABI。

```c
#include "zcs/sdk.h"

int main(void)
{
    zcs_server *s = zcs_server_create_from_ini("config.ini", NULL);
    if (!s || zcs_server_start(s) != ZN_OK)
        return 1;

    zcs_server_print_endpoints(s);
    zcs_server_install_default_signals(s);
    zcs_server_run(s, -1); /* Ctrl+C 退出 */

    zcs_server_stop(s);
    zcs_server_destroy(s);
    return 0;
}
```

链接方式与 `demo_media_server` 相同：`zcs` + `znet`。

嵌入和 API 分层的细节以 `include/zcs/sdk.h`、`include/zcs/server.h`、
`src/README.md` 以及头文件注释为准。能力矩阵、URL 规则、线程/鉴权/录制约定、
数据通路与安全传输，全部收在一份文档：[`docs/stream-pipeline.md`](docs/stream-pipeline.md)。

## 架构分层

目录重构已完成，按"共用媒体模型 + 明确分层"组织：

- `znet`：poller / 网络 / 线程 / 缓冲 / 日志，不认识媒体格式。
- `zmedia`：共用 packet 模型、codec 位流、容器、RTP/SDP、HLS/DASH。
- `zero-media-server`：应用协议、publish/play/vod/pull/record、GOP、时钟、运维。

server 内部：

| 目录 | 职责 |
|------|------|
| `core/` | frame / track / clock / event / GOP / 流注册表 |
| `media/` | codec / container / wire |
| `bridge/` | ZM ↔ ZCS 适配（唯一同时认识两边的层） |
| `protocol/` | RTMP / RTSP / HTTP / SRT / RTP / WebRTC 协议状态机与协议会话 |
| `publish/` | 推流接入编排（ingest、RTMP/RTP 适配） |
| `play/` | 播放分发与直播输出（HLS / DASH / HTTP-FLV） |
| `vod/` | 点播文件读取与 play lane |
| `pull/` | 拉流代理与播放器 |
| `egress/` | 出站泵、pacing、FLV/TS/RTP/MP4 输出 |
| `ops/` | server / config / webapi / webhook / module registry |

依赖方向：`core/media → bridge → egress → publish/play/vod/pull → protocol`，`ops` 在顶层。
跨层调用只允许经过声明的 port 头（如 `play/http_port.h`、
`core/stream/vod_source_port.h`）。CMake 按层拆成 `zcs_core/media/bridge/
protocol/roles/egress/ops/util` 对象库，最终聚合为 `zcs`。

RTMP/HTTP-FLV、VOD FLV 与 RTP ingest 全部走共用媒体模型（ZM）；

---

## 协议和地址

推流时用的名字，要和 URL 里的 `{stream}` 对上。同一路直播名再推一次，会踢掉原来的推流端，清掉该源的 GOP，由新的接上（不会两路同时往里写）。

| 方向 | 协议 | 示例 |
|------|------|------|
| 推 | RTMP | `rtmp://host:1935/live/{stream}` |
| 推 | RTSP RECORD | `rtsp://host:8554/live/{stream}` |
| 推 | SRT | `srt://host:9000?streamid=#!::r=live/{stream},m=publish` |
| 推 | WHIP | `POST http://host:8080/index/api/whip?app=live&stream={stream}` |
| 播 | RTSP / RTMP / HTTP-FLV / HLS / DASH / SRT / WHEP | 同一个 app / stream |

| 能力 | 编译 | 配置 |
|------|------|------|
| SRT | `-DZCS_ENABLE_SRT=ON` | `[protocol] enable_srt=1`，`[srt] port=` |
| WebRTC | **默认已编进去**（依赖内置，无需外部包）；不需要时 `-DZCS_ENABLE_WEBRTC=OFF` | `[webrtc] enable=1`，`[general] externIP=` 填外网能访问到的 IP |

---

## 打开 SRT / WebRTC

### Windows（全开）

依赖目录大致这样摆：

| 依赖 | 路径举例 | 里面要有 |
|------|----------|----------|
| libsrt | `D:\srt` | `include\srt\srt.h`、`lib\srt.lib`、`bin\srt.dll` |
| OpenSSL | `C:\Program Files\OpenSSL-Win64` | `include\openssl\ssl.h` |
| zice | 仓库自带 | `3rdpart/zice/` |

也可以照着 `cmake/build.local.cmake.example` 写成 `cmake/build.local.cmake`（这个文件已 gitignore）：

```powershell
cmake -S . -B build `
  -DZN_ENABLE_WEPOLL=ON `
  -DZCS_ENABLE_SRT=ON `
  -DZCS_ENABLE_WEBRTC=ON `
  -DZCS_WEBRTC_USE_ZICE=ON `
  -DZCS_SRT_ROOT="D:/srt" `
  -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"

cmake --build build --config Release
Copy-Item "D:\srt\bin\srt.dll" "build\examples\Release\"
```

配成功时，CMake 输出里大概会有：

```text
-- ZCS: SRT transport enabled (libsrt found)
-- ZCS: WebRTC zice enabled
-- ZCS: WebRTC DTLS-SRTP enabled (OpenSSL)
```

缺库只会打 WARNING，对应功能不会编进去。改过选项之后，重新跑一遍 `cmake -S . -B build ...` 再编译。

### Linux / macOS

```bash
# Debian/Ubuntu: sudo apt install libsrt-openssl-dev libssl-dev
# macOS: brew install srt openssl@3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DZCS_ENABLE_SRT=ON \
  -DZCS_ENABLE_WEBRTC=ON \
  -DZCS_WEBRTC_USE_ZICE=ON
cmake --build build -j
```

macOS 找不到 OpenSSL 时，加上 `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`。

### 常用 CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ZCS_ENABLE_SRT` | OFF | 打开 SRT（MPEG-TS），需要 libsrt |
| `ZCS_ENABLE_WEBRTC` | ON | 打开 WHIP / WHEP（依赖内置；关闭加 `-DZCS_ENABLE_WEBRTC=OFF`） |
| `ZCS_WEBRTC_USE_ZICE` | ON | 用仓库里的 zice |
| `ZCS_SRT_ROOT` | — | SRT 安装前缀（Windows 常用） |
| `OPENSSL_ROOT_DIR` | Windows 下常能自动找到 | OpenSSL 前缀 |
| `ZN_ENABLE_OPENSSL` | ON | TLS / WebRTC 的 DTLS |
| `ZN_BUILD_EXAMPLES` | ON | 是否编 demo |
| `ZCS_BUILD_TESTS` | OFF | 是否编生命周期等测试 |
| `ZN_ENABLE_WEPOLL` | ON | Windows 上的 Poller |

Windows 的 OpenSSL 可以用 [Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html)。

---

## 目录说明

```
include/zcs/     头文件树，与 src/ 同构（core / media / bridge / protocol / publish / play / vod / pull / egress / ops / util）
src/             实现：core / media / bridge / protocol / publish / play / vod / pull / egress / ops
examples/        demo_media_server、demo_gb28181_device、demo_onvif_probe
tests/           自动化测试（cmake -DZCS_BUILD_TESTS=ON 后 ctest）
conf/            config.ini 模板
cmake/           构建脚本，以及 build.local.cmake.example
3rdpart/         znet、zmedia（内置源码）
```

`demo_media_server` 是完整服务进程；`demo_gb28181_device` 用于 GB28181 设备端联调；
`demo_onvif_probe` 是 ONVIF 客户端自检工具。

3rdpart 为内置源码，不依赖 git submodule；更新 ZN / ZM 后重新编译：

```bash
cmake --build build --config Release   # 或 cmake --build build -j
```

---

## 许可证

以仓库里的 LICENSE 为准。协议和容器不少逻辑在 zmedia / znet 里；改媒体行为前，先分清是 ZCS 这一层，还是 kit 那一层。
