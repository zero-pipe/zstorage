/**
 * @file rtmp_server.h
 * @brief RTMP 服务：监听、会话分发与端口信息。
 */
#ifndef ZCS_SESSION_RTMP_SERVER_H
#define ZCS_SESSION_RTMP_SERVER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_rtmp_server zcs_rtmp_server;
struct zn_ssl_ctx;

typedef struct zcs_rtmp_server_opts {
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
    /** 非 NULL 且 ssl_port>0 时额外监听 RTMPS */
    struct zn_ssl_ctx *ssl_ctx;
    uint16_t ssl_port;
    /** TCP 会话首包/空闲超时（毫秒，0=关闭） */
    uint64_t handshake_timeout_ms;
    uint64_t idle_timeout_ms;
    /** 连接上限（0=不限）：单服务 / 单 IP */
    unsigned max_sessions;
    unsigned max_sessions_per_ip;
} zcs_rtmp_server_opts;

/**
 * 创建 RTMP 服务（poller_pool 必填）。ssl_ctx+ssl_port 同时有效时加开 RTMPS 监听。
 * @return 服务；opts/poller_pool 为空或 TCP 监听创建失败返回 NULL
 */
ZCS_API zcs_rtmp_server *zcs_rtmp_server_create(const zcs_rtmp_server_opts *opts);
/** 销毁服务及其底层 TCP/RTMPS 监听。 */
ZCS_API void zcs_rtmp_server_destroy(zcs_rtmp_server *srv);
/** 启动监听（RTMPS 监听失败仅告警，不影响主端口）。@return 主监听启动结果。 */
ZCS_API zn_err_t zcs_rtmp_server_start(zcs_rtmp_server *srv);
/** 停止主端口与 RTMPS 监听。 */
ZCS_API void zcs_rtmp_server_stop(zcs_rtmp_server *srv);
/** @return 实际绑定端口（opts.port 为 0 时由系统分配）；srv 无效返回 0。 */
ZCS_API uint16_t zcs_rtmp_server_port(const zcs_rtmp_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SESSION_RTMP_SERVER_H */
