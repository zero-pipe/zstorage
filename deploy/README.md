# ZCS 部署产物

面向真实运营环境的部署样例。

| 文件 | 用途 |
|------|------|
| `Dockerfile` | 多阶段构建镜像（debian:12-slim，`--build-arg` 控制 WebRTC/SRT） |
| `docker-compose.yml` | 本地/测试编排，含健康检查（`/index/api/system/health`）与 fd 上限 |
| `zcs.service` | systemd unit（`LimitNOFILE`、SIGHUP 重开日志、`NoNewPrivileges`） |

## Docker

```bash
# 基础镜像（RTMP/RTSP/HTTP-FLV/HLS/DASH）
docker build -t zcs:latest .

# 带 WebRTC（WHEP/WHIP）与 SRT
docker build --build-arg ZCS_ENABLE_WEBRTC=ON --build-arg ZCS_ENABLE_SRT=ON -t zcs:full .

docker run --rm -p 1935:1935 -p 8080:8080 -p 8554:8554 \
    -v "$PWD/conf:/etc/zcs:ro" -v "$PWD/record:/var/lib/zcs/record" \
    zcs:latest
```

- WebRTC 媒体走 UDP 端口段（默认 `[webrtc] port_min/port_max` = 50000–60000），
  容器部署需把该段一并映射，否则 ICE 连不通。
- 镜像以非 root 用户 `zcs` 运行。

## systemd

```bash
sudo useradd -r -s /sbin/nologin -d /var/lib/zcs zcs
sudo install -Dm755 build/examples/demo_media_server /usr/local/bin/zcs-server
sudo install -Dm644 conf/config.ini /etc/zcs/config.ini
sudo install -Dm644 deploy/zcs.service /etc/systemd/system/zcs.service
sudo mkdir -p /var/lib/zcs/record /var/log/zcs && sudo chown -R zcs:zcs /var/lib/zcs /var/log/zcs
sudo systemctl daemon-reload && sudo systemctl enable --now zcs
```

## 日志轮转

服务支持 SIGHUP 重新打开日志文件（`systemctl reload zcs`），可配合 logrotate 使用
copytruncate 或 rename+reload。
