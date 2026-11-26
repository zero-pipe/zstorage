/**
 * @file dispatch.c
 * @brief RTSP 播放分发：把 PLAY 接入 dispatch 端口（直播/VOD）。
 */
#include "zms/core/stream/hub.h"
#include "zms/play/dispatch.h"
#include "zms/media/container/demux_pipeline.h"
#include "session/rtsp/session_internal.h"
#include "zn/util/log.h"
#include <string.h>
static zn_err_t rtsp_protocol_on_play_live(void *session, zms_media_source *src,
                                            const zms_session_play_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    (void)opts;
    if (!rs || !src) {
        return ZN_ERR_INVALID;
    }
    if (zms_session_play_open_live(&rs->pl.src, src, ZMS_SESSION_LIVE_GOP) != ZN_OK) {
        return ZN_ERR_STATE;
    }
    rs->pl.gop_reader = rs->pl.src.readers.gop;
    return ZN_OK;
}

static zn_err_t rtsp_protocol_on_play_vod(void *session, zms_media_source *src,
                                           const zms_session_play_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    uint64_t seek_ms = opts ? opts->seek_ms : 0;
    if (!rs || !src || !zms_media_source_is_vod(src)) {
        return ZN_ERR_INVALID;
    }
    return zms_rtsp_session_play_vod_lane_attach(rs, src, seek_ms);
}

static zn_err_t rtsp_protocol_on_publish(void *session, zms_media_source *src,
                                          const zms_session_publish_opts *opts)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    (void)opts;
    if (!rs || !src || !rs->pb.ingress) {
        return ZN_ERR_INVALID;
    }
    return ZN_OK;
}

static void rtsp_protocol_on_teardown(void *session)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)session;
    if (!rs) {
        return;
    }
    zms_rtsp_session_egress_close(rs);
}

static const zms_session_dispatch_ops k_rtsp_dispatch = {
    .name = ZMS_SESSION_RTSP,
    .on_play_live = rtsp_protocol_on_play_live,
    .on_play_vod = rtsp_protocol_on_play_vod,
    .on_publish = rtsp_protocol_on_publish,
    .on_teardown = rtsp_protocol_on_teardown,
};

void zms_rtsp_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zms_session_dispatch_register(&k_rtsp_dispatch);
}
