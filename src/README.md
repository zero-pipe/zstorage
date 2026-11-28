# ZCS 源码目录

目录重构已完成。允许的依赖方向由 CMake 对象库与 port 头约束，
跨层调用只允许经过声明的 port 头。完整的代码地图、加代码放哪、
命名约定见 [`docs/stream-pipeline.md`](../docs/stream-pipeline.md)。

| 目录 | 职责 |
|------|------|
| `core/` | frame / track / clock / event / GOP ring / 流注册表 |
| `media/` | codec / container / wire |
| `bridge/` | ZM ↔ ZCS：`zm_mem` 包装 `zn_buf`、track/codec 映射；**唯一同时认识两边的层** |
| `protocol/` | 应用协议状态机与协议会话：`rtmp/ rtsp/ http/ srt/ rtp/ webrtc/` |
| `publish/` | 推流接入编排：`ingest/`、RTMP/RTP 适配 |
| `play/` | 播放分发与直播输出：`hls/ dash/ http_flv/` |
| `vod/` | 点播文件读取（`io/`）与 play lane（`play/`） |
| `pull/` | 拉流代理与播放器 |
| `egress/` | 出站泵、pacing、FLV/TS/RTP/MP4 输出 |
| `ops/` | server、config、webapi、webhook、module registry（组合根） |
| `util/` | 通用工具 |

依赖方向：

```text
core/media -> bridge -> egress -> publish/play/vod/pull -> protocol
ops 在顶层
```

禁止：协议层 include 角色实现内部、`publish`/`play`/`vod` include `protocol/`、`core` include
`protocol/`/`publish/`/`play/`/`vod/`/`pull/`/`egress/`、`media` include `core/` 及以上、
ZM 依赖 ZN 或 ZCS。

## 协议层边界

协议 / 会话 / 业务归 ZCS，媒体格式与无状态协议规范归 ZM。

- RTMP：`protocol/rtmp/` 自研握手 / chunk / AMF / netconnection / netstream，ZM 不再带 `librtmp`。
- RTSP：`protocol/rtsp/` 拥有方法 / 会话 / Transport / interleaved。
- SDP 语法、RTP 包格式与打包器在 ZM（`libsdp`、`librtp`、`zm_rtp_writer`）。

## 共用媒体模型

ZM 提供唯一的跨层对象：

- `zm_packet`：ES 载荷 + PTS/DTS(ms) + codec/track + flags + `es_format`
- `zm_mem`：与分配器解耦的载荷句柄；ZCS 用 `zn_buf` 零拷贝包装
- `zm_track` / `zm_config`：归一化的 AVCC/HVCC/VVCC/AV1C/ASC/OpusHead
- `zm_reader` / `zm_writer`：每个容器格式一个接口；codec 是数据，不是文件
- `zm_packager`：HLS/DASH 分段与清单

**一个能力只有一个实现**：一个 FLV writer、一个 HLS packager、一个 TS muxer。

## 头文件树

`include/zcs/` 与 `src/` 分层同构：`include/zcs/<layer>/...`。`ZCS_API` 标注
稳定导出符号；嵌入入口是 `zcs/sdk.h`，领域聚合头为 `zcs/live.h`、`zcs/vod.h`、
`zcs/player.h`、`zcs/protocol.h`、`zcs/zcs.h`。

## 媒体路径

- RTMP publish 与 pull-proxy：`zm_flv_reader`。
- 直播/点播 FLV 输出：`zm_flv_writer`（直播经 `zcs_egress_pipeline`，
  点播经 `zcs_vod_flv_muxer`，最终同一个 writer）。
- RTP ingest（RTSP RECORD/client、WebRTC WHIP）：`zm_rtp_ingest`。
- RTP 输出（RTSP/WebRTC）：`egress/rtp/rtp_payload_muxer` 内部用 `zm_rtp_writer`
  打包；SSRC/seq/时间线与 RTCP SR 调度在 ZCS 会话层。
- 直播 HLS(TS)：`zm_hls_packager`（H.264/H.265 + AAC）；其他音频或
  fMP4 codec（VP8/VP9/AV1/H266）仍走各自的 writer。
- 直播 DASH(fMP4)：`zm_dash_packager`（H.264/H.265 + AAC）；其他组合
  保留 legacy DASH 路径。
- VOD 文件 demux：MP4/MOV 与 MKV 经 `zm_mov_reader` / `zm_mkv_reader`
  接口（`src/bridge/zm_file_demuxer.c` 适配为 `zcs_container` demuxer）；
  FLV-file / MPEG 仍走旧 `zcs_container` 文件 demuxer（ZM 接口暂缺 seek/config）。
