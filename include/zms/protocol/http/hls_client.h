/**
 * @file hls_client.h
 * @brief HTTP-HLS 客户端。
 */
#ifndef ZMS_PROTOCOL_HTTP_HLS_CLIENT_H
#define ZMS_PROTOCOL_HTTP_HLS_CLIENT_H

/**
 * HLS 拉流客户端（TS 切片 + m3u8 刷新；fMP4 待补） */
#include "zms/zms_export.h"
#include "zms/core/frame.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_ssl_ctx;
typedef struct zn_poller zn_poller;
typedef struct zn_ssl_ctx zn_ssl_ctx;

typedef struct zms_http_hls_client zms_http_hls_client;

typedef void (*zms_http_hls_client_on_ready_cb)(void *user);
typedef void (*zms_http_hls_client_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_http_hls_client_on_error_cb)(zn_err_t err, void *user);

typedef struct zms_http_hls_client_opts {
    zn_poller *poller;
    /** http(s)://index.m3u8 */
    const char *url;
    zn_ssl_ctx *ssl_ctx;
    zms_http_hls_client_on_ready_cb on_ready;
    zms_http_hls_client_on_frame_cb on_frame;
    zms_http_hls_client_on_error_cb on_error;
    void *user;
} zms_http_hls_client_opts;

/**
 * 创建 HLS 拉流客户端。poller、url、on_frame 必填；https 需 ssl_ctx。
 * @return 客户端；参数缺失/URL 解析或 TLS 不可用返回 NULL
 */
ZMS_API zms_http_hls_client *zms_http_hls_client_create(const zms_http_hls_client_opts *opts);
/** 停止并释放客户端及其内部 HTTP/TS 解封装资源。 */
ZMS_API void zms_http_hls_client_destroy(zms_http_hls_client *c);
/** 开始播放：拉取 m3u8、按序拉取切片并回调 on_frame。@return ZN_OK（异步进行）。 */
ZMS_API zn_err_t zms_http_hls_client_play(zms_http_hls_client *c);
/** 停止拉流并关闭当前 HTTP 连接（客户端可再次 play 复用）。 */
ZMS_API void zms_http_hls_client_stop(zms_http_hls_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_HTTP_HLS_CLIENT_H */
