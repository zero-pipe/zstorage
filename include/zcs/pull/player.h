#ifndef ZCS_PULL_PLAYER_H
#define ZCS_PULL_PLAYER_H

/**
 * @file player.h
 * @brief 统一拉流播放器 SDK 接口。
 *
 * 嵌入 ZCS 的应用可用本 API 拉取远端流并接收归一化轨/帧，供解码器与渲染集成。
 */
#include "zcs/core/frame.h"
#include "zcs/core/media_track.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_ssl_ctx;
typedef struct zn_poller zn_poller;
typedef struct zn_ssl_ctx zn_ssl_ctx;

typedef struct zcs_player zcs_player;

typedef void (*zcs_player_on_ready_cb)(void *user);
typedef void (*zcs_player_on_track_cb)(const zcs_media_track *track, void *user);
typedef void (*zcs_player_on_frame_cb)(const zcs_frame *frame, void *user);
typedef void (*zcs_player_on_error_cb)(zn_err_t err, void *user);

typedef struct zcs_player_opts {
    zn_poller *poller;
    /** RTSP(S)、HTTP(S)-FLV、HTTP(S)-HLS URL。RTMP 帧模式尚未实现。 */
    const char *url;
    /** TLS 协议在底层客户端需要 TLS 上下文时必填。 */
    zn_ssl_ctx *ssl_ctx;
    /** 仅 RTSP；AUTO 默认 TCP，支持时 UDP 回退。 */
    zcs_rtsp_rtp_mode rtsp_rtp_mode;
    int retry_count;
    int reconnect_delay_ms;
    zcs_player_on_ready_cb on_ready;
    zcs_player_on_track_cb on_track;
    zcs_player_on_frame_cb on_frame;
    zcs_player_on_error_cb on_error;
    void *user;
} zcs_player_opts;

/**
 * 创建拉流播放器，按 URL 协议自动选择后端（RTSP/HTTP-FLV/HLS；RTMP 帧模式暂未实现）。
 * opts 会被复制；poller 与 url 必填，创建时会注册全部模块。
 * @return 播放器实例；参数非法、协议不支持或底层客户端创建失败返回 NULL。用 destroy 释放。
 */
ZCS_API zcs_player *zcs_player_create(const zcs_player_opts *opts);
/** 停止并销毁播放器（释放底层协议客户端）；对 NULL 安全。 */
ZCS_API void zcs_player_destroy(zcs_player *p);
/**
 * 开始/恢复播放。失败时同步触发 on_error 回调并返回错误。
 * @return ZN_OK；后端不支持（如 RTMP）返回 ZN_ERR_NOT_IMPL；实例为空返回 ZN_ERR_INVALID。
 */
ZCS_API zn_err_t zcs_player_play(zcs_player *p);
/** 停止播放但保留实例，可再次 play；对 NULL 安全。 */
ZCS_API void zcs_player_stop(zcs_player *p);
/** 返回当前后端协议名字面量（借用，勿释放）；未知后端返回 NULL。 */
ZCS_API const char *zcs_player_protocol(const zcs_player *p);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PULL_PLAYER_H */
