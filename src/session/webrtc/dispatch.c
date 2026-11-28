/**
 * @file dispatch.c
 * @brief WebRTC 播放分发注册。
 */
#include "zcs/play/dispatch.h"
static zn_err_t webrtc_protocol_on_play_live(void *session, zcs_media_source *src,
                                              const zcs_session_play_opts *opts)
{
    zcs_egress_source *play = (zcs_egress_source *)session;
    (void)opts;
    if (!play || !src) {
        return ZN_ERR_INVALID;
    }
    return zcs_session_play_open_live(play, src, ZCS_SESSION_LIVE_GOP);
}

static void webrtc_protocol_on_teardown(void *session)
{
    zcs_egress_source *play = (zcs_egress_source *)session;
    zcs_session_play_close(play);
}

static const zcs_session_dispatch_ops k_webrtc_dispatch = {
    .name = ZCS_SESSION_WEBRTC,
    .on_play_live = webrtc_protocol_on_play_live,
    .on_play_vod = NULL,
    .on_publish = NULL,
    .on_teardown = webrtc_protocol_on_teardown,
};

void zcs_webrtc_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zcs_session_dispatch_register(&k_webrtc_dispatch);
}
