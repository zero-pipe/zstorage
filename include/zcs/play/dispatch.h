#ifndef ZCS_PLAY_DISPATCH_H
#define ZCS_PLAY_DISPATCH_H

/**
 * @file dispatch.h
 * @brief Session 分发：URL/会话枢纽解复用与出站管线。
 */
#include "zcs/core/stream/hub.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_SESSION_RTMP "rtmp"
#define ZCS_SESSION_RTSP "rtsp"
#define ZCS_SESSION_HTTP_FLV "http-flv"
#define ZCS_SESSION_HTTP_TS "http-ts"
#define ZCS_SESSION_WEBRTC "webrtc"
#define ZCS_SESSION_SRT "srt"
#define ZCS_SESSION_RTP_PS "rtp-ps"

typedef enum zcs_session_live_mode {
    ZCS_SESSION_LIVE_GOP = 0,
    ZCS_SESSION_LIVE_KEY = 1,
    ZCS_SESSION_LIVE_EDGE = 2,
} zcs_session_live_mode;

typedef struct zcs_session_play_opts {
    const char *player;
    uint64_t seek_ms;
} zcs_session_play_opts;

typedef struct zcs_session_publish_opts {
    const char *schema;
} zcs_session_publish_opts;

typedef struct zcs_session_dispatch_ops {
    const char *name;
    zn_err_t (*on_play_live)(void *session, zcs_media_source *src,
                              const zcs_session_play_opts *opts);
    zn_err_t (*on_play_vod)(void *session, zcs_media_source *src,
                             const zcs_session_play_opts *opts);
    zn_err_t (*on_publish)(void *session, zcs_media_source *src,
                            const zcs_session_publish_opts *opts);
    void (*on_teardown)(void *session);
} zcs_session_dispatch_ops;

/**
 * 注册（或同名替换）一种协议的 session 分发 ops。容量上限 12。
 * 内部只保存指针，ops 必须具有静态/进程级生命周期。
 */
ZCS_API void zcs_session_dispatch_register(const zcs_session_dispatch_ops *ops);
/** 注册所有内建协议（RTMP/RTSP/HTTP/HTTP-TS/WebRTC/RTP-PS/SRT）；幂等，仅首次生效。 */
ZCS_API void zcs_session_dispatch_register_all(void);

/** 按协议名查找分发 ops（借用指针）；会自动触发 register_all；未找到返回 NULL。 */
ZCS_API const zcs_session_dispatch_ops *zcs_session_dispatch_find(const char *name);

/** 统一直播读者附着（GOP / live_key / live_edge）。 */
ZCS_API zn_err_t zcs_session_play_open_live(zcs_egress_source *play, zcs_media_source *src,
                                             zcs_session_live_mode mode);

/**
 * 为点播源在 egress_source 上打开 VOD 读端；seek_ms>0 时顺带定位。
 * 打开失败直接返回错误；seek 失败被忽略（尽力定位）。
 */
ZCS_API zn_err_t zcs_session_play_open_vod(zcs_egress_source *play, zcs_media_source *src,
                                            uint64_t seek_ms);

/** 关闭 egress_source 的读端与资源；对 NULL 安全。 */
ZCS_API void zcs_session_play_close(zcs_egress_source *play);

/** 按协议名分发播放（直播 / VOD 自动分支）。 */
ZCS_API zn_err_t zcs_session_attach_play(const char *protocol, void *session,
                                          zcs_media_source *src, const zcs_session_play_opts *opts);

/** 按协议名调用 on_teardown 解绑播放会话；协议未注册或无回调时无操作。 */
ZCS_API void zcs_session_detach_play(const char *protocol, void *session);

/** 按协议名分发推流会话到 on_publish；协议未注册或无回调时返回错误。 */
ZCS_API zn_err_t zcs_session_attach_publish(const char *protocol, void *session,
                                             zcs_media_source *src,
                                             const zcs_session_publish_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_DISPATCH_H */
