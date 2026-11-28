# ZCS 部署产物

面向真实运营环境的部署样例。

| 文件 | 用途 |
|------|------|
| `Dockerfile` | 多阶段构建镜像（debian:12-slim） |
| `docker-compose.yml` | 本地/测试编排，含健康检查（`/index/api/system/health`）与 fd 上限 |
| `zcs.service` | systemd unit（`LimitNOFILE`、SIGHUP 重开日志、`NoNewPrivileges`） |

## Docker

```bash
docker build -t zcs:latest .

docker run --rm -p 8180:8180 -p 8654:8654 \
    -v "$PWD/conf:/etc/zcs:ro" -v "$PWD/www/zstorage:/var/lib/zcs/storage" \
    zcs:latest
```

端口按 `conf/config.ini` 中的 `[http] port`、`[rtsp] port` 调整；镜像以非 root 用户 `zcs` 运行。

## systemd

```bash
sudo useradd -r -s /sbin/nologin -d /var/lib/zcs zcs
sudo install -Dm755 build/examples/zcs /usr/local/bin/zcs
sudo install -Dm644 conf/config.ini /etc/zcs/config.ini
sudo install -Dm644 deploy/zcs.service /etc/systemd/system/zcs.service
sudo mkdir -p /var/lib/zcs/storage /var/log/zcs && sudo chown -R zcs:zcs /var/lib/zcs /var/log/zcs
sudo systemctl daemon-reload && sudo systemctl enable --now zcs
```

## 日志轮转

服务支持 SIGHUP 重新打开日志文件（`systemctl reload zcs`），可配合 logrotate 使用
copytruncate 或 rename+reload。
