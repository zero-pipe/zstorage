/**
 * @file flv_client.h
 * @brief HTTP-FLV 客户端。
 */
#ifndef ZMS_PROTOCOL_HTTP_FLV_CLIENT_H
#define ZMS_PROTOCOL_HTTP_FLV_CLIENT_H

/**
 * HTTP(S)-FLV 拉流客户端：GET + FLV tag 解析。
 * 归一化入站：on_media 原样 tag 交给 channel.input_rtmp_*（与 RTMP 拉流一致）。
 * on_frame：经 flv_tag_demuxer 解为 ES（仅 demo/legacy）。
 */
#include "zms/zms_export.h"
#include "zms/core/frame.h"
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

typedef struct zms_http_flv_client zms_http_flv_client;

typedef void (*zms_http_flv_client_on_ready_cb)(void *user);
typedef void (*zms_http_flv_client_on_media_cb)(uint8_t msg_type, uint32_t tag_dts_ms,
                                                const void *body, size_t len, void *user);
typedef void (*zms_http_flv_client_on_frame_cb)(const zms_frame *frame, void *user);
typedef void (*zms_http_flv_client_on_error_cb)(zn_err_t err, void *user);

typedef struct zms_http_flv_client_opts {
    zn_poller *poller;
    /** http:// https://host[:port]/path…flv */
    const char *url;
    /** https 时必填 */
    zn_ssl_ctx *ssl_ctx;
    zms_http_flv_client_on_ready_cb on_ready;
    /** 推荐：与 rtmp_client.on_media 相同，走 channel FLV/RTMP 入站 */
    zms_http_flv_client_on_media_cb on_media;
    /** 可选：ES 帧（on_media 二选一或仅 legacy 使用 on_frame） */
    zms_http_flv_client_on_frame_cb on_frame;
    zms_http_flv_client_on_error_cb on_error;
    void *user;
} zms_http_flv_client_opts;

/**
 * 创建 HTTP(S)-FLV 拉流客户端（不发起连接）。on_media 与 on_frame 至少提供一个；
 * 提供 on_frame 时内部建 FLV tag demuxer。https 需 ssl_ctx。
 * @return 客户端对象；参数缺失、URL 非法、TLS 条件不满足或内存不足返回 NULL
 */
ZMS_API zms_http_flv_client *zms_http_flv_client_create(const zms_http_flv_client_opts *opts);
/** 停止并销毁客户端，释放 HTTP/FLV 解析器与传输层；对 NULL 安全。 */
ZMS_API void zms_http_flv_client_destroy(zms_http_flv_client *c);
/** 发起 GET 请求（异步，结果经 on_ready/on_media/on_frame/on_error 回调）。 */
ZMS_API zn_err_t zms_http_flv_client_play(zms_http_flv_client *c);
/** 断开连接、复位 HTTP 与 FLV 解析状态；之后可再次 play。对 NULL 安全。 */
ZMS_API void zms_http_flv_client_stop(zms_http_flv_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_HTTP_FLV_CLIENT_H */
