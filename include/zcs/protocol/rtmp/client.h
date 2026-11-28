/**
 * @file client.h
 * @brief RTMP 客户端。
 */
#ifndef ZCS_PROTOCOL_RTMP_CLIENT_H
#define ZCS_PROTOCOL_RTMP_CLIENT_H

/**
 * RTMP 拉流客户端（play）：接收音视频 FLV tag，经回调交给 channel。
 */
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_ssl_ctx;
typedef struct zn_poller zn_poller;
typedef struct zn_ssl_ctx zn_ssl_ctx;

typedef struct zcs_rtmp_client zcs_rtmp_client;

typedef void (*zcs_rtmp_client_on_ready_cb)(void *user);
typedef void (*zcs_rtmp_client_on_media_cb)(uint8_t msg_type, uint32_t tag_dts_ms, const void *body,
                                            size_t len, void *user);
typedef void (*zcs_rtmp_client_on_error_cb)(zn_err_t err, void *user);

typedef struct zcs_rtmp_client_opts {
    zn_poller *poller;
    /** rtmp:// rtmps://host[:port]/app/stream[?query] */
    const char *url;
    /** rtmps 时必填（通常由 zcs_pull_ssl_ctx 注入） */
    zn_ssl_ctx *ssl_ctx;
    zcs_rtmp_client_on_ready_cb on_ready;
    zcs_rtmp_client_on_media_cb on_media;
    zcs_rtmp_client_on_error_cb on_error;
    void *user;
} zcs_rtmp_client_opts;

/**
 * 拉流 URL → RTMP connect app + play 名（与 ZCS RTMP 服务约定一致）。
 * rtmp://host/live/stream/id → app=live/stream play=id
 * rtmp://host/live/proxied/stream/id → app=live/proxied play=stream/id
 */
ZCS_API zn_err_t zcs_rtmp_parse_pull_url(const char *url, char *connect_app, size_t app_cap,
                                          char *play_name, size_t play_cap);

/**
 * 创建 RTMP 拉流客户端（不发起连接）。内部解析 URL 并建立 TCP/TLS 客户端。
 * rtmps 需 ssl_ctx，未编译 OpenSSL 时返回 NULL。
 * @return 客户端对象；opts/poller/url 缺失、URL 非法、TLS 条件不满足或内存不足返回 NULL
 */
ZCS_API zcs_rtmp_client *zcs_rtmp_client_create(const zcs_rtmp_client_opts *opts);
/** 停止并销毁客户端，释放协议对象与缓冲；对 NULL 安全。 */
ZCS_API void zcs_rtmp_client_destroy(zcs_rtmp_client *c);
/** 重置状态并发起 connect/握手序列（异步，结果经 on_ready/on_media/on_error 回调）。 */
ZCS_API zn_err_t zcs_rtmp_client_play(zcs_rtmp_client *c);
/** 断开连接、销毁协议对象并复位到握手初始态；之后可再次 play。对 NULL 安全。 */
ZCS_API void zcs_rtmp_client_stop(zcs_rtmp_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTMP_CLIENT_H */
