/**
 * @file session_vod.c
 * @brief RTSP 点播：play lane 打开、seek 与追帧。
 */
#include "zms/play/vod_play_port.h"
#include "session/rtsp/session_internal.h"
#include "zn/platform.h"
#include "zms/play/dispatch.h"
#include "zms/core/stream/hub.h"
#include "zms/util/thread_pool.h"
#include "zms/core/stream/buffer.h"
#include "zms/media/clock.h"
#include "zms/core/event.h"
#include "zms/core/gop/queue.h"
#include "zms/protocol/rtsp/parser.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

zn_err_t zms_rtsp_session_play_vod_lane_attach(zms_rtsp_session *rs, zms_media_source *src,
                                                uint64_t seek_ms)
{
    uint64_t play_ms;

    if (!rs || !src) {
        return ZN_ERR_INVALID;
    }
    if (!rs->pl.vod_lane) {
        rs->pl.vod_lane = zms_vod_play_lane_open(src, rs->poller);
        if (!rs->pl.vod_lane) {
            return ZN_ERR_STATE;
        }
        rs->pl.vod_reader = zms_vod_play_lane_buffer_reader(rs->pl.vod_lane);
        if (!rs->pl.vod_reader) {
            zms_vod_play_lane_close(rs->pl.vod_lane);
            rs->pl.vod_lane = NULL;
            return ZN_ERR_STATE;
        }
    }

    play_ms = zms_vod_play_lane_seek_ms(rs->pl.vod_lane, seek_ms);
    rs->pl.seek_ms = play_ms;
    zms_vod_play_lane_prefill(rs->pl.vod_lane);
    zms_vod_play_lane_align_reader(rs->pl.vod_lane);
    rs->pl.vod_reader = zms_vod_play_lane_buffer_reader(rs->pl.vod_lane);
    return rs->pl.vod_reader ? ZN_OK : ZN_ERR_STATE;
}

static int zms_rtsp_session_alive(zms_rtsp_session *rs)
{
    return rs && rs->tcp && zn_tcp_session_user(rs->tcp) == rs;
}

static void rtsp_vod_play_flush_stale(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    if (rs->pl.sender) {
        zms_rtp_play_sender_reset(rs->pl.sender);
    }
    if (rs->tcp) {
        zn_tcp_session_out_discard(rs->tcp);
    }
}

void zms_rtsp_session_vod_play_continue(zms_rtsp_session *rs, int is_replay, int was_paused,
                                        int did_seek, uint64_t seek_ms, double scale,
                                        const char *range_req, int range_parsed, int range_now)
{
    zms_rtp_play_vod_seek_state vst;
    uint32_t rtp_anchor_ms;

    if (!is_replay) {
        zms_rtsp_session_play_mux_create(rs);
    } else if (did_seek) {
        rtsp_vod_play_flush_stale(rs);
        rs->pl.rtcp_boot_sent = 0;
        rs->pl.rtcp_video_sr_sent = 0;
        rs->pl.rtcp_audio_sr_sent = 0;
    }

    if (did_seek && rs->pl.vod_reader) {
        zms_gop_slot slot;
        if (zms_vod_buffer_reader_peek_muxed(rs->pl.vod_reader, &slot)) {
            zn_info("RTSP #%u seek ready: fifo_head ts=%u track=%d key=%d", rs->session_no,
                     (unsigned)slot.dts_ms, (int)slot.track, slot.keyframe);
        }
    }

    memset(&vst, 0, sizeof(vst));
    vst.mux = rs->pl.rtp_muxer;
    vst.sender = rs->pl.sender;
    vst.vod_reader = rs->pl.vod_reader;
    vst.seek_ms = seek_ms;
    vst.scale = scale;
    vst.is_replay = is_replay;
    vst.was_paused = was_paused;
    vst.did_seek = did_seek;
    zms_rtp_play_apply_vod_seek(&vst);

    if (is_replay && did_seek && rs->pl.rtp_muxer) {
        uint32_t boot_anchor = (uint32_t)seek_ms;

        if (rs->pl.vod_reader) {
            boot_anchor = zms_rtp_play_vod_anchor_ms(rs->pl.vod_reader, boot_anchor);
        }
        (void)zms_rtp_play_bootstrap_vod(rs->pl.rtp_muxer, rs->source, rs->pl.vod_reader, rs->session_no,
                                         rs->pl.vod_lane, boot_anchor);
    }

    {
        char extra[ZMS_RTSP_PLAY_EXTRA_MAX];
        char rtp_info[ZMS_RTSP_RTP_INFO_MAX];
        double vod_dur_sec = zms_rtsp_session_vod_duration_ms(rs) / 1000.0;

        rtp_anchor_ms = (uint32_t)seek_ms;
        if (did_seek && rs->pl.vod_reader) {
            rtp_anchor_ms = zms_rtp_play_vod_anchor_ms(rs->pl.vod_reader, rtp_anchor_ms);
        }
        {
            zms_rtsp_play_rtp_info_args rtp_args =
                zms_rtsp_session_rtp_info_args(rs, rtp_anchor_ms, 1);
            zms_rtsp_play_format_rtp_info(&rtp_args, rtp_info, sizeof(rtp_info));
        }
        zms_rtsp_play_format_vod_play_200(extra, sizeof(extra), rs->session_id, scale, seek_ms,
                                          vod_dur_sec, rtp_info);
        zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
        zn_info(
            "RTSP #%u PLAY 200: app=%s stream=%s video=%d audio=%d vod=1 transport=%s scale=%.2f",
            rs->session_no, rs->app, rs->stream, rs->source->has_video, rs->source->has_audio,
            rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp", scale);
        zn_info("RTSP #%u PLAY vod: replay=%d paused=%d range=%s parsed=%d now=%d seek_ms=%llu "
                 "did_seek=%d lane=1",
                 rs->session_no, is_replay, was_paused, range_req ? range_req : "(none)",
                 range_parsed, range_now, (unsigned long long)seek_ms, did_seek);
    }
    rs->pl.config_pending = 0;
    if (rs->source && !rs->pl.reader_attached) {
        zms_media_source_reader_add(rs->source);
        rs->play_start_ms = zn_monotonic_ms();
        zms_media_event_play(rs->source, "rtsp");
        rs->pl.reader_attached = 1;
    }
    zms_rtsp_session_play_kick(rs);
    if (is_replay && did_seek) {
        zms_rtsp_session_play_try_rtcp_boot(rs);
    }
    if (rs->rtp_mode == ZMS_RTSP_RTP_UDP && rs->pl.sender) {
        zms_rtp_play_sender_flush(rs->pl.sender, (int)ZMS_RTP_PLAY_RTP_FLUSH, 0);
    }
}

static void rtsp_vod_open_blocking(void *user)
{
    zms_rtsp_vod_play_ctx *job = (zms_rtsp_vod_play_ctx *)user;
    uint64_t play_ms;

    job->lane = zms_vod_play_lane_open(job->src, NULL);
    if (!job->lane) {
        return;
    }
    play_ms = zms_vod_play_lane_seek_ms(job->lane, job->seek_ms);
    zms_vod_play_lane_prefill(job->lane);
    zms_vod_play_lane_align_reader(job->lane);
    if (!zms_vod_play_lane_buffer_reader(job->lane)) {
        zms_vod_play_lane_close(job->lane);
        job->lane = NULL;
        return;
    }
    job->seek_ms = play_ms;
}

static void rtsp_vod_open_on_io(void *user)
{
    zms_rtsp_vod_play_ctx *job = (zms_rtsp_vod_play_ctx *)user;
    zms_rtsp_session *rs = job->rs;

    if (!zms_rtsp_session_alive(rs)) {
        zms_vod_play_lane_close(job->lane);
        free(job);
        return;
    }
    if (!job->lane) {
        zn_warn("RTSP PLAY 404: vod open async failed app=%s stream=%s", rs->app, rs->stream);
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        free(job);
        return;
    }
    if (rs->poller) {
        zms_vod_reader_bind_poller_lite(zms_vod_play_lane_reader(job->lane), rs->poller);
    }
    rs->pl.vod_lane = job->lane;
    rs->pl.vod_reader = zms_vod_play_lane_buffer_reader(job->lane);
    rs->pl.seek_ms = job->seek_ms;
    job->lane = NULL;
    zms_rtsp_session_vod_play_continue(
        rs, job->is_replay, job->was_paused, job->did_seek, job->seek_ms, job->scale,
        job->range_req[0] ? job->range_req : NULL, job->range_parsed, job->range_now);
    free(job);
}

int zms_rtsp_session_vod_open_try_async(zms_rtsp_vod_play_ctx *job)
{
    zn_poller *pol;

    if (!zms_vod_thread_pool_enabled() || !job || !job->rs || job->rs->pl.vod_lane) {
        return 0;
    }
    pol = job->rs->poller ? job->rs->poller : zn_tcp_session_poller(job->rs->tcp);
    if (!pol) {
        return 0;
    }
    if (zms_vod_thread_pool_run(pol, rtsp_vod_open_blocking, rtsp_vod_open_on_io, job) != ZN_OK) {
        return 0;
    }
    return 1;
}
