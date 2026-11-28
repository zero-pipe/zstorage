# Zero Cloud Storage (ZCS)

ZCS 是零云存储组件：按通道主动拉流，把视频以 MPEG-PS 写进预分配的槽位文件（`.zslot`），提供录像查询与 RTSP 回放。回放只走 RTSP，播放器拖动进度条通过 stop + start 重建会话。

## 定位

- **做**：PS 录像落盘、关键帧索引、录像查询/删除、节点状态上报、RTSP 回放、平台回调（webhook）。
- **不做**：MP4 点播/录制、HLS/DASH/HTTP-FLV 播放、WebRTC、SRT、ONVIF、GB28181。相关模块已从代码和配置中裁剪。

依赖的 vendored 库（保持原有前缀，未改名）：`3rdpart/znet`、`3rdpart/zbase`、`3rdpart/zmedia`。

## 构建与运行

```bash
cmake -S . -B build -DZN_BUILD_EXAMPLES=ON
cmake --build build --config Release

# Windows
.\build\examples\Release\zcs.exe --config .\conf\config.ini
# Linux
./build/examples/zcs --config conf/config.ini
```

配置见 `conf/config.ini.example`。核心段：

- `[storage]`：槽位目录、容量、保留天数、槽位大小、拉流兜底地址。
- `[rtsp]`：RTSP 服务端口（回放出口）。
- `[hook]`：平台回调地址（节点注册、心跳、封口上报）。

## HTTP API

录像与查询：

| 接口 | 说明 |
|------|------|
| `/index/api/zstorage/record/start` | 创建录制任务（channel + 可选 rtsp_url） |
| `/index/api/zstorage/record/stop` | 停止录制任务并封口 |
| `/index/api/zstorage/record/list` | 录制任务列表 |
| `/index/api/zstorage/record/query` | 按通道和时间段查询录像片段 |
| `/index/api/zstorage/record/delete` | 按槽位号删除录像 |
| `/index/api/zstorage/node/status` | 节点状态（容量、用量、活跃任务） |

回放：

| 接口 | 说明 |
|------|------|
| `/index/api/zstorage/playback/start` | 定位到片段并注册 VOD 源，返回 RTSP 流名；`start` 为段内相对毫秒 |
| `/index/api/zstorage/playback/stop` | 停止回放源 |

拖动进度条：先 `playback/stop`，再带新的 `start` 调 `playback/start`，不提供运行中 seek。

## 回放链路

```
playback/start → vod_source_open(.zslot) → file_reader → zslot_demuxer
  → MPEG-PS 解复用 → H.264/H.265/AAC → rtp_play_pump → RTSP 出流
```

录像链路：

```
record/start → 拉流代理(RTSP) → live_ingest → gop_queue
  → egress_zs_recorder → zs_writer → 槽位文件 + 关键帧索引
  → 封口回调上报平台
```

## 测试

```bash
ctest --test-dir build -C Release -LE integration
```
