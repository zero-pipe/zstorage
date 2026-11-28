# 示例程序

只保留一个入口：

| 可执行文件 | 源码 | 说明 |
|------------|------|------|
| `zcs` | [zcs.c](zcs.c) | 启动录像/回放服务，加载配置、初始化 poller、注册 WebAPI |

[demo_server_runtime.c](demo_server_runtime.c) 提供配置加载与运行时初始化，供 zcs 入口复用。

## 运行

```bash
cp conf/config.ini.example config.ini
./build/examples/zcs --config config.ini
# Windows: .\build\examples\Release\zcs.exe --config .\conf\config.ini
```

启动后日志会打印实际监听的 RTSP/HTTP/RTMP 端口。录像与回放通过 HTTP API 控制，回放播放器用日志里的 RTSP 地址拉流。

`--log file` 可同时写日志文件；`--seconds N` 运行 N 秒后自动退出（联调用）。
