# ─────────────────────────────────────────────────────────────────────────────
# ZCS (Zero Cloud Storage) 多阶段构建镜像
#
# 构建：
#   docker build -t zcs:latest .
# 运行：
#   docker run --rm -p 8180:8180 -p 8654:8654 \
#              -v $PWD/conf:/etc/zcs -v $PWD/www/zstorage:/var/lib/zcs/storage zcs:latest
# ─────────────────────────────────────────────────────────────────────────────
FROM debian:12-slim AS build

ARG ZCS_BUILD_TESTS=OFF

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DZN_BUILD_EXAMPLES=ON \
        -DZCS_BUILD_TESTS=${ZCS_BUILD_TESTS} \
    && cmake --build build --parallel "$(nproc)"

# ── 运行镜像 ─────────────────────────────────────────────────────────────────
FROM debian:12-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r zcs && useradd -r -g zcs -d /var/lib/zcs -s /sbin/nologin zcs

COPY --from=build /src/build/examples/zcs /usr/local/bin/zcs
COPY conf/config.ini /etc/zcs/config.ini

RUN mkdir -p /var/lib/zcs/storage /var/log/zcs \
    && chown -R zcs:zcs /var/lib/zcs /var/log/zcs

USER zcs
WORKDIR /var/lib/zcs

# HTTP(WebAPI) / RTSP(回放) / RTMP(拉流接入)
EXPOSE 8180 8654 2035

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/zcs"]
CMD ["--config", "/etc/zcs/config.ini", "--log", "/var/log/zcs/zcs.log"]
