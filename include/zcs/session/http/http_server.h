/**
 * @file http_server.h
 * @brief HTTP 服务：监听、连接分发与路由挂载。
 */
#ifndef ZCS_SESSION_HTTP_SERVER_H
#define ZCS_SESSION_HTTP_SERVER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_server zcs_http_server;

struct zcs_config;
struct zn_ssl_ctx;

typedef struct zcs_http_server_opts {
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
    /** [api] secret，对应 WebApi CHECK_SECRET；空=仅本机可访问 WebAPI */
    const char *api_secret;
    /** HTTP API / WebHook；可为 NULL */
    const struct zcs_config *cfg;
    /** 非 NULL 且 ssl_port>0 时额外监听 HTTPS/WSS */
    struct zn_ssl_ctx *ssl_ctx;
    uint16_t ssl_port;
    /** TCP 会话首包/空闲超时（毫秒，0=关闭） */
    uint64_t handshake_timeout_ms;
    uint64_t idle_timeout_ms;
    /** 连接上限（0=不限）：单服务 / 单 IP */
    unsigned max_sessions;
    unsigned max_sessions_per_ip;
} zcs_http_server_opts;

/**
 * 创建 HTTP 监听（不开始 accept）。内部注册会话分发；cfg.webrtc.enable 时一并创建 WebRTC 服务；
 * ssl_ctx 与 ssl_port 同时给出时额外创建 HTTPS/WSS 监听（失败仅告警）。
 * @return 服务器对象；opts 或 poller_pool 缺失、内存不足返回 NULL
 */
ZCS_API zcs_http_server *zcs_http_server_create(const zcs_http_server_opts *opts);
/** 销毁 WebRTC 服务与监听并释放对象；不负责停止运行中的循环，应先 stop。对 NULL 安全。 */
ZCS_API void zcs_http_server_destroy(zcs_http_server *srv);
/** 开始 accept（含 HTTPS/WSS）；主监听失败返回错误码，TLS 失败仅告警不致命。 */
ZCS_API zn_err_t zcs_http_server_start(zcs_http_server *srv);
/** 停止 accept（含 HTTPS/WSS），不影响已建立会话的处理。 */
ZCS_API void zcs_http_server_stop(zcs_http_server *srv);
/** 取主监听实际绑定端口（opts.port=0 时用于获取动态分配的端口）；NULL 返回 0。 */
ZCS_API uint16_t zcs_http_server_port(const zcs_http_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SESSION_HTTP_SERVER_H */
