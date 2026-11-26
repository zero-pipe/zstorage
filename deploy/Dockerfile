# ─────────────────────────────────────────────────────────────────────────────
# ZMS（zero-media-server）多阶段构建镜像
#
# 构建：
#   docker build -t zms:latest .
#   docker build --build-arg ZMS_ENABLE_WEBRTC=ON --build-arg ZMS_ENABLE_SRT=ON -t zms:full .
# 运行：
#   docker run --rm -p 1935:1935 -p 8080:8080 -p 8554:8554 \
#              -v $PWD/conf:/etc/zms -v $PWD/record:/var/lib/zms/record zms:latest
# ─────────────────────────────────────────────────────────────────────────────
FROM debian:12-slim AS build

ARG ZMS_ENABLE_WEBRTC=OFF
ARG ZMS_ENABLE_SRT=OFF
ARG ZMS_BUILD_TESTS=OFF

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        libssl-dev libsrt-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DZN_BUILD_EXAMPLES=ON \
        -DZMS_ENABLE_WEBRTC=${ZMS_ENABLE_WEBRTC} \
        -DZMS_ENABLE_SRT=${ZMS_ENABLE_SRT} \
        -DZMS_BUILD_TESTS=${ZMS_BUILD_TESTS} \
    && cmake --build build --parallel "$(nproc)"

# ── 运行镜像 ─────────────────────────────────────────────────────────────────
FROM debian:12-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libsrt1.5-openssl ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r zms && useradd -r -g zms -d /var/lib/zms -s /sbin/nologin zms

COPY --from=build /src/build/examples/demo_media_server /usr/local/bin/zms-server
COPY conf/config.ini /etc/zms/config.ini

RUN mkdir -p /var/lib/zms/record /var/log/zms \
    && chown -R zms:zms /var/lib/zms /var/log/zms

USER zms
WORKDIR /var/lib/zms

# RTMP / HTTP(API+FLV+HLS) / RTSP
EXPOSE 1935 8080 8554
# WebRTC 媒体端口段（与 [webrtc] port_min/port_max 保持一致）
EXPOSE 50000-60000/udp

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/zms-server"]
CMD ["--config", "/etc/zms/config.ini", "--log", "/var/log/zms/zms.log"]
