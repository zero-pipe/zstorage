/**
 * @file client_http.h
 * @brief WebRTC client 用最小 HTTP(S) POST（WHIP/WHEP 信令）。
 */
#ifndef ZCS_SRC_SESSION_WEBRTC_CLIENT_HTTP_H
#define ZCS_SRC_SESSION_WEBRTC_CLIENT_HTTP_H

#include "zn/poller/poller.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_ssl_ctx;
typedef struct zcs_http_client zcs_http_client;

typedef void (*zcs_http_client_cb)(int status, const char *location, const char *body,
                                   size_t body_len, void *user);

/**
 * 发起 POST；回调在 poller 线程触发一次（成功或失败，status=0 表示传输失败）。
 * token 可为 NULL；带 token 时添加 Authorization: Bearer。
 */
zcs_http_client *zcs_http_client_post(zn_poller *poller, const char *url, const char *content_type,
                                      const char *body, size_t body_len, const char *token,
                                      struct zn_ssl_ctx *ssl_ctx, int timeout_ms,
                                      zcs_http_client_cb cb, void *user);
/** 取消并销毁；不会触发回调。 */
void zcs_http_client_destroy(zcs_http_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_SESSION_WEBRTC_CLIENT_HTTP_H */
