/**
 * @file dispatch.c
 * @brief RTSP 播放分发：把 PLAY 接入 dispatch 端口（直播/VOD）。
 */
#include "zcs/core/stream/hub.h"
#include "zcs/play/dispatch.h"
#include "zcs/media/container/demux_pipeline.h"
#include "session/rtsp/session_internal.h"
#include "zn/util/log.h"
#include <string.h>
static zn_err_t rtsp_protocol_on_play_live(void *session, zcs_media_source *src,
                                            const zcs_session_play_opts *opts)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)session;
    (void)opts;
    if (!rs || !src) {
        return ZN_ERR_INVALID;
    }
    if (zcs_session_play_open_live(&rs->pl.src, src, ZCS_SESSION_LIVE_GOP) != ZN_OK) {
        return ZN_ERR_STATE;
    }
    rs->pl.gop_reader = rs->pl.src.readers.gop;
    return ZN_OK;
}

static zn_err_t rtsp_protocol_on_play_vod(void *session, zcs_media_source *src,
                                           const zcs_session_play_opts *opts)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)session;
    uint64_t seek_ms = opts ? opts->seek_ms : 0;
    if (!rs || !src || !zcs_media_source_is_vod(src)) {
        return ZN_ERR_INVALID;
    }
    return zcs_rtsp_session_play_vod_lane_attach(rs, src, seek_ms);
}

static zn_err_t rtsp_protocol_on_publish(void *session, zcs_media_source *src,
                                          const zcs_session_publish_opts *opts)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)session;
    (void)opts;
    if (!rs || !src || !rs->pb.ingress) {
        return ZN_ERR_INVALID;
    }
    return ZN_OK;
}

static void rtsp_protocol_on_teardown(void *session)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)session;
    if (!rs) {
        return;
    }
    zcs_rtsp_session_egress_close(rs);
}

static const zcs_session_dispatch_ops k_rtsp_dispatch = {
    .name = ZCS_SESSION_RTSP,
    .on_play_live = rtsp_protocol_on_play_live,
    .on_play_vod = rtsp_protocol_on_play_vod,
    .on_publish = rtsp_protocol_on_publish,
    .on_teardown = rtsp_protocol_on_teardown,
};

void zcs_rtsp_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zcs_session_dispatch_register(&k_rtsp_dispatch);
}
