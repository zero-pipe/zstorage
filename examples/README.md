# 示例程序

只保留三个入口：综合媒体服务、GB28181 设备端联调、ONVIF 客户端自检。

| 可执行文件 | 源码 | 说明 |
|------------|------|------|
| `demo_media_server` | [demo_media_server.c](demo_media_server.c) | 综合媒体服务：RTMP/RTSP/SRT 接入，HTTP-FLV/HLS/DASH/RTSP 播放，点播与 HTTP API |
| `demo_gb28181_device` | [demo_gb28181_device.c](demo_gb28181_device.c) | GB28181 设备端联调：RTMP 收流 + SIP 注册 + PS/RTP 推流 |
| `demo_onvif_probe` | [demo_onvif_probe.c](demo_onvif_probe.c) | ONVIF 客户端自检：WS-Discovery、GetProfiles、GetStreamUri |

`demo_media_server` 与 `demo_gb28181_device` 共用
[demo_server_runtime.c](demo_server_runtime.c) 做配置加载与运行时初始化。

## demo_media_server

```bash
cp conf/config.ini config.ini
./build/examples/demo_media_server --config config.ini
# Windows: build\examples\Release\demo_media_server.exe --config config.ini
```

推流 `rtmp://host:1935/live/{stream}` 或 RTSP RECORD 后，按启动日志打印的
RTSP / HTTP-FLV / HLS / DASH 地址播放。`--log file` 可同时写日志文件。

## demo_gb28181_device

```bash
./build/examples/demo_gb28181_device --config tests/gb28181/config.ini --seconds 60
```

配置见 [`docs/stream-pipeline.md`](../docs/stream-pipeline.md)。

## demo_onvif_probe

```bash
# 组播发现
./build/examples/demo_onvif_probe --discover

# 直接读取指定设备的 profile 与 RTSP 地址
./build/examples/demo_onvif_probe --addr http://192.168.1.64/onvif/device_service \
    --user admin --pass '***'
```
