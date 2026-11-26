# ZMS 部署产物

面向真实运营环境的部署样例。

| 文件 | 用途 |
|------|------|
| `Dockerfile` | 多阶段构建镜像（debian:12-slim，`--build-arg` 控制 WebRTC/SRT） |
| `docker-compose.yml` | 本地/测试编排，含健康检查（`/index/api/system/health`）与 fd 上限 |
| `zms.service` | systemd unit（`LimitNOFILE`、SIGHUP 重开日志、`NoNewPrivileges`） |

## Docker

```bash
# 基础镜像（RTMP/RTSP/HTTP-FLV/HLS/DASH）
docker build -t zms:latest .

# 带 WebRTC（WHEP/WHIP）与 SRT
docker build --build-arg ZMS_ENABLE_WEBRTC=ON --build-arg ZMS_ENABLE_SRT=ON -t zms:full .

docker run --rm -p 1935:1935 -p 8080:8080 -p 8554:8554 \
    -v "$PWD/conf:/etc/zms:ro" -v "$PWD/record:/var/lib/zms/record" \
    zms:latest
```

- WebRTC 媒体走 UDP 端口段（默认 `[webrtc] port_min/port_max` = 50000–60000），
  容器部署需把该段一并映射，否则 ICE 连不通。
- 镜像以非 root 用户 `zms` 运行。

## systemd

```bash
sudo useradd -r -s /sbin/nologin -d /var/lib/zms zms
sudo install -Dm755 build/examples/demo_media_server /usr/local/bin/zms-server
sudo install -Dm644 conf/config.ini /etc/zms/config.ini
sudo install -Dm644 deploy/zms.service /etc/systemd/system/zms.service
sudo mkdir -p /var/lib/zms/record /var/log/zms && sudo chown -R zms:zms /var/lib/zms /var/log/zms
sudo systemctl daemon-reload && sudo systemctl enable --now zms
```

## 日志轮转

服务支持 SIGHUP 重新打开日志文件（`systemctl reload zms`），可配合 logrotate 使用
copytruncate 或 rename+reload。
