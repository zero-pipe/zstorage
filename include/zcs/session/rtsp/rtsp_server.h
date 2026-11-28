/**
 * @file rtsp_server.h
 * @brief RTSP 服务：监听、会话分发与端口信息。
 */
#ifndef ZCS_SESSION_RTSP_SERVER_H
#define ZCS_SESSION_RTSP_SERVER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_rtsp_server zcs_rtsp_server;
struct zn_ssl_ctx;

typedef struct zcs_rtsp_server_opts {
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const char *host;
    uint16_t port;
    /** SDP o= / c= 对外公告地址；NULL 则回退到 extern_ip / 127.0.0.1 */
    const char *advertise_host;
    /** 非 NULL 且 ssl_port>0 时额外监听 RTSPS */
    struct zn_ssl_ctx *ssl_ctx;
    uint16_t ssl_port;
    /** TCP 会话首包/空闲超时（毫秒，0=关闭） */
    uint64_t handshake_timeout_ms;
    uint64_t idle_timeout_ms;
    /** 连接上限（0=不限）：单服务 / 单 IP */
    unsigned max_sessions;
    unsigned max_sessions_per_ip;
} zcs_rtsp_server_opts;

/**
 * 创建 RTSP 监听（不开始 accept）。内部注册各协议会话分发、记录 SDP 公告地址；
 * ssl_ctx 与 ssl_port 同时给出时额外创建 RTSPS 监听（失败仅告警）。
 * @return 服务器对象；opts 或 poller_pool 缺失、内存不足返回 NULL
 */
ZCS_API zcs_rtsp_server *zcs_rtsp_server_create(const zcs_rtsp_server_opts *opts);
/** 销毁监听（含 RTSPS）并释放对象；不负责停止已运行的循环，应先 stop。对 NULL 安全。 */
ZCS_API void zcs_rtsp_server_destroy(zcs_rtsp_server *srv);
/** 开始 accept（含 RTSPS）；主监听失败返回错误码，RTSPS 失败仅告警不致命。 */
ZCS_API zn_err_t zcs_rtsp_server_start(zcs_rtsp_server *srv);
/** 停止 accept（含 RTSPS），不影响已建立会话的处理。 */
ZCS_API void zcs_rtsp_server_stop(zcs_rtsp_server *srv);
/** 取主监听实际绑定端口（opts.port=0 时用于获取动态分配的端口）；NULL 返回 0。 */
ZCS_API uint16_t zcs_rtsp_server_port(const zcs_rtsp_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SESSION_RTSP_SERVER_H */
