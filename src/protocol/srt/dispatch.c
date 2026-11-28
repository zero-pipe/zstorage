/**
 * @file dispatch.c
 * @brief SRT 分发：接入 play/publish 端口。
 */
#include "zcs/play/dispatch.h"
#include "zcs/play/play_binding.h"
#include "zcs/media/container/demux_pipeline.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/egress/mpegts/mpegts_egress.h"
#include "server_internal.h"
#include "zn/util/log.h"
#include <string.h>
static void srt_protocol_on_h264_ps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                    size_t pps_len, void *user)
{
    zcs_srt_session *s = (zcs_srt_session *)user;
    if (!s || !s->ingress || !sps || !pps || sps_len == 0 || pps_len == 0) {
        return;
    }
    if (zcs_live_ingest_set_h264_sps_pps(s->ingress, sps, sps_len, pps, pps_len) != ZN_OK) {
        zn_warn("SRT #%d H264 config apply failed (sps=%u pps=%u)", s->session_no,
                 (unsigned)sps_len, (unsigned)pps_len);
    }
}

static void srt_protocol_on_frame(const zcs_frame *frame, void *user)
{
    zcs_srt_session *s = (zcs_srt_session *)user;
    if (!s || !s->ingress || !frame || frame->size == 0) {
        return;
    }
    (void)zcs_live_ingest_input_frame(s->ingress, frame);
}

static zn_err_t srt_protocol_on_publish(void *session, zcs_media_source *src,
                                         const zcs_session_publish_opts *opts)
{
    zcs_srt_session *s = (zcs_srt_session *)session;
    zcs_demux_pipeline_opts pcfg;
    (void)opts;
    (void)src;
    if (!s || !s->ingress) {
        return ZN_ERR_INVALID;
    }
    if (s->publish_pipeline) {
        return ZN_OK;
    }
    /* MPEG-TS PTS/DTS 为 90kHz tick；时间线走标准 ingress（同 RTSP RECORD）。 */
    zcs_live_ingest_set_rtp_clocks(s->ingress, 90000, ZCS_CODEC_AAC, 90000);
    /* libmpeg 损坏/丢包边缘后 TS 可间隙 >300ms；默认 stamp clamp 会破坏 GOP 间距。 */
    zcs_live_ingest_set_stamp_max_delta(s->ingress, 10000);
    /* PES A/V PTS 原点不同（如 audio dts=0、video dts=1566）；RTP 式 300ms clamp 会把 video 归零。 */
    zcs_live_ingest_set_stamp_av_clamp(s->ingress, 0);
    /* demux PTS/DTS 已在 MPEG-TS 轴上为 ms；勿按 track 重归零。 */
    zcs_live_ingest_set_timeline_linear_ms(s->ingress, 1);
    /* 推迟 ring video_config 至 IDR，避免缓存 IDR 前 P/B 帧。 */
    zcs_live_ingest_set_defer_gop_vcfg(s->ingress, 1);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.container = ZCS_CONTAINER_MPEGTS;
    pcfg.on_frame = srt_protocol_on_frame;
    pcfg.on_mpegts_h264_ps = srt_protocol_on_h264_ps;
    pcfg.user = s;
    s->publish_pipeline = zcs_demux_pipeline_create(&pcfg);
    return s->publish_pipeline ? ZN_OK : ZN_ERR_STATE;
}

static zn_err_t srt_protocol_on_play_live(void *session, zcs_media_source *src,
                                           const zcs_session_play_opts *opts)
{
    zcs_srt_session *s = (zcs_srt_session *)session;
    (void)opts;
    if (!s || !src) {
        return ZN_ERR_INVALID;
    }
    return zcs_session_play_open_live(&s->play, src, ZCS_SESSION_LIVE_GOP);
}

static void srt_protocol_on_teardown(void *session)
{
    zcs_srt_session *s = (zcs_srt_session *)session;
    zcs_play_binding bind;

    if (!s) {
        return;
    }
    if (s->live_muxer) {
        zcs_mpegts_egress_destroy(s->live_muxer);
        s->live_muxer = NULL;
    }
    if (s->mode == ZCS_SRT_SESSION_MODE_PLAY) {
        memset(&bind, 0, sizeof(bind));
        bind.source = &s->source;
        bind.play = &s->play;
        bind.reader_attached = &s->reader_attached;
        bind.play_start_ms = &s->play_start_ms;
        bind.player = "srt";
        zcs_play_binding_close_readers(&bind);
    }
    if (s->publish_pipeline) {
        (void)zcs_demux_pipeline_flush(s->publish_pipeline);
        zcs_demux_pipeline_destroy(s->publish_pipeline);
        s->publish_pipeline = NULL;
    }
}

static const zcs_session_dispatch_ops k_srt_dispatch = {
    .name = ZCS_SESSION_SRT,
    .on_play_live = srt_protocol_on_play_live,
    .on_play_vod = NULL,
    .on_publish = srt_protocol_on_publish,
    .on_teardown = srt_protocol_on_teardown,
};

void zcs_srt_register(void)
{
    static int registered; /* 启动阶段，单线程 */
    if (registered) {
        return;
    }
    registered = 1;
    zcs_session_dispatch_register(&k_srt_dispatch);
}
