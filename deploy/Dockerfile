# ─────────────────────────────────────────────────────────────────────────────
# ZCS（zero-media-server）多阶段构建镜像
#
# 构建：
#   docker build -t zcs:latest .
#   docker build --build-arg ZCS_ENABLE_WEBRTC=ON --build-arg ZCS_ENABLE_SRT=ON -t zcs:full .
# 运行：
#   docker run --rm -p 1935:1935 -p 8080:8080 -p 8554:8554 \
#              -v $PWD/conf:/etc/zcs -v $PWD/record:/var/lib/zcs/record zcs:latest
# ─────────────────────────────────────────────────────────────────────────────
FROM debian:12-slim AS build

ARG ZCS_ENABLE_WEBRTC=OFF
ARG ZCS_ENABLE_SRT=OFF
ARG ZCS_BUILD_TESTS=OFF

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
        -DZCS_ENABLE_WEBRTC=${ZCS_ENABLE_WEBRTC} \
        -DZCS_ENABLE_SRT=${ZCS_ENABLE_SRT} \
        -DZCS_BUILD_TESTS=${ZCS_BUILD_TESTS} \
    && cmake --build build --parallel "$(nproc)"

# ── 运行镜像 ─────────────────────────────────────────────────────────────────
FROM debian:12-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libsrt1.5-openssl ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r zcs && useradd -r -g zcs -d /var/lib/zcs -s /sbin/nologin zcs

COPY --from=build /src/build/examples/demo_media_server /usr/local/bin/zcs-server
COPY conf/config.ini /etc/zcs/config.ini

RUN mkdir -p /var/lib/zcs/record /var/log/zcs \
    && chown -R zcs:zcs /var/lib/zcs /var/log/zcs

USER zcs
WORKDIR /var/lib/zcs

# RTMP / HTTP(API+FLV+HLS) / RTSP
EXPOSE 1935 8080 8554
# WebRTC 媒体端口段（与 [webrtc] port_min/port_max 保持一致）
EXPOSE 50000-60000/udp

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/zcs-server"]
CMD ["--config", "/etc/zcs/config.ini", "--log", "/var/log/zcs/zcs.log"]
