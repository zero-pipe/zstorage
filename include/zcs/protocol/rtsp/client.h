/**
 * @file client.h
 * @brief RTSP 客户端。
 */
#ifndef ZCS_PROTOCOL_RTSP_CLIENT_H
#define ZCS_PROTOCOL_RTSP_CLIENT_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zcs/core/frame.h"
#include "zcs/core/media_track.h"
#include "transport.h"
#include "sdp.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_tcp_client;
struct zn_ssl_ctx;
typedef struct zn_poller zn_poller;
typedef struct zn_ssl_ctx zn_ssl_ctx;

typedef void (*zcs_rtsp_client_on_ready_cb)(void *user);
typedef void (*zcs_rtsp_client_on_track_cb)(const zcs_media_track *track, void *user);
typedef void (*zcs_rtsp_client_on_frame_cb)(const zcs_frame *frame, void *user);
typedef void (*zcs_rtsp_client_on_error_cb)(zn_err_t err, void *user);

typedef struct zcs_rtsp_client zcs_rtsp_client;

typedef struct zcs_rtsp_client_opts {
    zn_poller *poller;
    /** rtsp:// rtsps:// */
    const char *url;
    /** rtsps 时必填（通常由 zcs_pull_ssl_ctx 注入） */
    zn_ssl_ctx *ssl_ctx;
    /** RTP 传输：AUTO（先 TCP，SETUP 461 时回退 UDP）/ TCP / UDP */
    zcs_rtsp_rtp_mode rtp_mode;
    /** 可选；未设时从 url user:pass@ 解析 */
    const char *username;
    const char *password;
    /** -1=无限重试 */
    int retry_count;
    int reconnect_delay_ms;
    zcs_rtsp_client_on_ready_cb on_ready;
    zcs_rtsp_client_on_track_cb on_track;
    zcs_rtsp_client_on_frame_cb on_frame;
    zcs_rtsp_client_on_error_cb on_error;
    void *user;
} zcs_rtsp_client_opts;

/**
 * 创建 RTSP 拉流客户端（分配 splitter、RTP 接收器与 TCP/TLS 连接，尚不发起连接）。
 * opts->poller 与 url 必填；rtsps 需提供 ssl_ctx。rtp_mode 中 AUTO 先 TCP，
 * SETUP 461 时回退 UDP。retry_count==0 归一化为 -1（无限重试）。
 * @return 客户端；缺参或 TLS 不可用时返回 NULL（用 destroy 释放）。
 */
ZCS_API zcs_rtsp_client *zcs_rtsp_client_create(const zcs_rtsp_client_opts *opts);
/** 停止并销毁客户端，释放连接、splitter 与接收器；会先发 TEARDOWN。 */
ZCS_API void zcs_rtsp_client_destroy(zcs_rtsp_client *c);
/** 开始拉流：复位状态并异步发起连接；后续通过回调上报就绪/轨道/帧/错误。 */
ZCS_API zn_err_t zcs_rtsp_client_play(zcs_rtsp_client *c);
/** 停止拉流：取消重试、发送 TEARDOWN、释放 UDP，并延时关闭连接。幂等。 */
ZCS_API void zcs_rtsp_client_stop(zcs_rtsp_client *c);
/** 取最近一次 DESCRIBE 解析出的 SDP 会话（借用，随客户端生命周期有效）。 */
ZCS_API const zcs_sdp_session *zcs_rtsp_client_session(const zcs_rtsp_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_CLIENT_H */
