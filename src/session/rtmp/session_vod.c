/**
 * @file session_vod.c
 * @brief RTMP 点播路径：lane 打开/挂接，以及 NetStream onpause/onseek/onplayctrl/getStreamLength。
 *        归属 vod（src/vod/play）；由 RTMP NetStream 命令分发与直播 session 核心驱动。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/play/vod_play_port.h"
#include "session/rtmp/session_internal.h"
#include "zcs/play/dispatch.h"
#include "zcs/egress/pacing/egress_live_policy.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/clock.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/util/thread_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct zcs_rtmp_vod_lane_ctx {
    zcs_rtmp_session *s;
    zcs_media_source *src;
    zcs_vod_play_lane *lane;
    uint64_t start_ms;
    unsigned token;
} zcs_rtmp_vod_lane_ctx;

/** RTMP 点播 catchup：seek/resume 后预填 client buffer */
#define ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK 96u

static uint32_t rtmp_netstream_ms_arg(double val, const zcs_media_source *src)
{
    uint64_t dur_ms = 0;
    uint64_t ms;

    if (val <= 0.0) {
        return 0;
    }
    ms = (uint64_t)(val + 0.5);
    if (src && zcs_media_source_is_vod(src)) {
        dur_ms = zcs_vod_source_duration_ms(src);
    }
    if (dur_ms > 0 && ms > dur_ms) {
        ms = dur_ms;
    }
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void zcs_rtmp_session_vod_pump_hold(zcs_rtmp_session *s, int hold)
{
    if (s && s->pl.vod_lane) {
        zcs_vod_play_lane_set_pump_hold(s->pl.vod_lane, hold);
    }
}

static void zcs_rtmp_session_vod_set_play_scale(zcs_rtmp_session *s, double scale)
{
    if (!s || scale <= 0.0 || scale > 16.0) {
        return;
    }
    zcs_egress_clock_set_scale(&s->pl.clk, scale);
    zn_info("[rtmp] vod_scale session=%u scale=%.3f", s->session_no, scale);
}

static void zcs_rtmp_session_vod_save_pause_pos(zcs_rtmp_session *s)
{
    zcs_gop_slot slot;

    if (!s || !s->pl.vod_reader) {
        return;
    }
    if (zcs_vod_buffer_reader_peek_muxed(s->pl.vod_reader, &slot)) {
        s->pl.vod_pause_ms = slot.dts_ms;
        s->pl.vod_pause_pos_valid = 1;
    }
}

static void zcs_rtmp_session_vod_resume_after_seek(zcs_rtmp_session *s)
{
    if (!s) {
        return;
    }
    zcs_rtmp_session_vod_pump_hold(s, 0);
    if (zcs_egress_clock_is_paused(&s->pl.clk)) {
        zcs_egress_clock_resume(&s->pl.clk);
    }
}

zn_err_t zcs_rtmp_session_play_vod_lane_attach(zcs_rtmp_session *s, zcs_media_source *src,
                                                uint64_t seek_ms)
{
    zn_poller *pol;
    uint64_t play_ms = 0;

    if (!s || !src) {
        return ZN_ERR_INVALID;
    }
    pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
    if (!s->pl.vod_lane) {
        s->pl.vod_lane = zcs_vod_play_lane_open(src, pol);
        if (!s->pl.vod_lane) {
            return ZN_ERR_STATE;
        }
    }
    if (pol) {
        zcs_vod_reader_bind_poller_lite(zcs_vod_play_lane_reader(s->pl.vod_lane), pol);
    }
    if (seek_ms) {
        play_ms = zcs_vod_play_lane_seek_ms(s->pl.vod_lane, seek_ms);
    } else {
        (void)zcs_vod_play_lane_prepare(s->pl.vod_lane, 0);
    }
    zcs_vod_play_lane_prefill(s->pl.vod_lane);
    zcs_vod_play_lane_align_reader(s->pl.vod_lane);
    s->pl.vod_reader = zcs_vod_play_lane_buffer_reader(s->pl.vod_lane);
    if (!s->pl.vod_reader) {
        return ZN_ERR_STATE;
    }
    s->pl.vod_last_seek_ms = play_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)play_ms;
    s->pl.gop_reader = NULL;
    /* lane 持有 fifo reader；勿单独 detach play.readers.vod。 */
    s->pl.src.readers.gop = NULL;
    s->pl.src.readers.vod = NULL;
    zcs_egress_source_close(&s->pl.src);
    s->pl.src.source = src;
    s->pl.src.is_live = 0;
    s->pl.src.readers.vod = s->pl.vod_reader;
    return ZN_OK;
}

int zcs_rtmp_session_vod_attach(zcs_rtmp_session *s, uint64_t seek_ms)
{
    zcs_session_play_opts pcfg;

    if (!s || !s->source) {
        return 0;
    }
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZCS_SESSION_RTMP;
    pcfg.seek_ms = seek_ms;
    return zcs_session_attach_play(ZCS_SESSION_RTMP, s, s->source, &pcfg) == ZN_OK;
}

static uint32_t rtmp_vod_buffer_anchor_ms(zcs_vod_buffer_reader *rd, uint32_t ms)
{
    zcs_gop_slot slot;

    if (rd && zcs_vod_buffer_reader_peek_muxed(rd, &slot)) {
        return slot.dts_ms;
    }
    return ms;
}

static void zcs_rtmp_session_vod_do_seek(zcs_rtmp_session *s, uint32_t ms, int flush_after)
{
    size_t lag;
    int same_pos;

    if (!s || !s->source || !zcs_media_source_is_vod(s->source)) {
        return;
    }
    if (!s->pl.vod_lane && !zcs_rtmp_session_vod_attach(s, ms)) {
        zn_warn("[rtmp] seek_failed session=%u reason=vod_lane_unavailable", s->session_no);
        return;
    }
    zcs_rtmp_session_vod_resume_after_seek(s);

    same_pos = (ms == s->pl.vod_last_seek_ms);
    if (same_pos) {
        lag = s->pl.vod_reader ? zcs_vod_buffer_reader_lag(s->pl.vod_reader) : 0;
        if (lag > ZCS_EGRESS_CATCHUP_LAG) {
            s->pl.vod_catchup = (int)ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
            zn_debug("[rtmp] seek_dedupe session=%u ms=%u lag=%zu", s->session_no, (unsigned)ms,
                      lag);
            if (flush_after && s->state == ZCS_RTMP_SESSION_STATE_PLAYING &&
                !s->pl.boot_pending) {
                zcs_rtmp_session_play_flush_nolock(s);
            }
            return;
        }
        zn_debug("[rtmp] seek_refresh session=%u ms=%u fifo_lag=%zu", s->session_no, (unsigned)ms,
                  lag);
    } else {
        s->pl.vod_last_seek_ms = ms;
        zcs_mux_av_timeline_reset(&s->pl.timeline);
    }

    ms = (uint32_t)zcs_vod_play_lane_seek_ms(s->pl.vod_lane, ms);
    zcs_vod_play_lane_prefill(s->pl.vod_lane);
    zcs_vod_play_lane_align_reader(s->pl.vod_lane);
    ms = rtmp_vod_buffer_anchor_ms(s->pl.vod_reader, ms);
    zcs_egress_clock_rebase(&s->pl.clk, ms);
    s->pl.video_armed = 0;
    s->pl.logged_video = 0;
    s->pl.logged_audio = 0;
    s->pl.vod_catchup = (int)ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    zn_info("[rtmp] seek session=%u ms=%u", s->session_no, (unsigned)ms);
    if (flush_after && s->state == ZCS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending) {
        zcs_rtmp_session_play_flush_nolock(s);
    }
}

int zcs_rtmp_session_vod_replay_seek(zcs_rtmp_session *s, uint64_t start_ms)
{
    uint32_t seek_ms;

    if (!s || !s->pl.vod_lane) {
        return -1;
    }
    seek_ms = start_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)start_ms;
    zcs_rtmp_session_vod_do_seek(s, seek_ms, 0);
    zcs_rtmp_session_send_data_onstatus(s, "NetStream.Play.Reset", "Resetting and playing.");
    zcs_rtmp_session_send_data_onstatus(s, "NetStream.Seek.Notify", "Seeking.");
    zcs_rtmp_session_send_data_onstatus(s, "NetStream.Play.Start", "Start video on demand.");
    zcs_rtmp_session_play_flush_nolock(s);
    zcs_rtmp_session_flush_tcp(s);
    return 0;
}

void zcs_rtmp_session_play_kick_vod_prime(zcs_rtmp_session *s, uint64_t start_ms)
{
    uint32_t kick_ms = start_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)start_ms;

    if (!s || !s->pl.vod_lane) {
        return;
    }
    if (start_ms) {
        s->pl.vod_last_seek_ms = kick_ms;
        s->pl.video_armed = 0;
    }
    s->pl.vod_catchup = (int)ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    zcs_egress_clock_arm(&s->pl.clk);
    (void)zcs_egress_clock_lock_epoch(&s->pl.clk, kick_ms);
    zcs_mux_av_timeline_reset(&s->pl.timeline);
}

static void rtmp_vod_lane_open_blocking(void *user)
{
    zcs_rtmp_vod_lane_ctx *job = (zcs_rtmp_vod_lane_ctx *)user;

    if (!job) {
        return;
    }
    job->lane = zcs_vod_play_lane_open(job->src, NULL);
    if (!job->lane) {
        return;
    }
    if (job->start_ms) {
        (void)zcs_vod_play_lane_seek_ms(job->lane, job->start_ms);
    } else {
        (void)zcs_vod_play_lane_prepare(job->lane, 0);
    }
    zcs_vod_play_lane_prefill(job->lane);
    zcs_vod_play_lane_align_reader(job->lane);
    if (!zcs_vod_play_lane_buffer_reader(job->lane)) {
        zcs_vod_play_lane_close(job->lane);
        job->lane = NULL;
    }
}

static void rtmp_vod_lane_open_on_io(void *user)
{
    zcs_rtmp_vod_lane_ctx *job = (zcs_rtmp_vod_lane_ctx *)user;
    zcs_rtmp_session *s;
    zn_poller *pol;
    uint64_t start_ms;

    if (!job) {
        return;
    }
    s = job->s;
    start_ms = job->start_ms;
    if (!zcs_rtmp_session_alive(s) || !s || s->destroy_token != job->token ||
        s->destroy_scheduled) {
        if (job->lane) {
            zcs_vod_play_lane_close(job->lane);
        }
        free(job);
        return;
    }
    if (!job->lane) {
        zn_warn("[rtmp] vod_lane_open_failed session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        zcs_rtmp_session_lock(s);
        if (!s->destroy_scheduled && s->pl.boot_pending) {
            zcs_rtmp_session_play_fail(s);
        }
        zcs_rtmp_session_unlock(s);
        free(job);
        return;
    }
    pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
    s->pl.vod_lane = job->lane;
    job->lane = NULL;
    free(job);

    zcs_rtmp_session_lock(s);
    if (!s->destroy_scheduled && s->pl.boot_pending) {
        if (zcs_rtmp_session_vod_attach(s, start_ms)) {
            zcs_rtmp_session_play_kick_vod_prime(s, start_ms);
            zcs_rtmp_session_play_kick_finish(s);
        } else {
            zcs_rtmp_session_play_fail(s);
        }
    }
    zcs_rtmp_session_unlock(s);
    (void)pol;
}

int zcs_rtmp_session_vod_lane_try_async(zcs_rtmp_session *s, uint64_t start_ms)
{
    zn_poller *pol;
    zcs_rtmp_vod_lane_ctx *job;

    if (!zcs_vod_thread_pool_enabled() || !s || s->pl.vod_lane) {
        return 0;
    }
    pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
    if (!pol) {
        return 0;
    }
    job = (zcs_rtmp_vod_lane_ctx *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->s = s;
    job->src = s->source;
    job->start_ms = start_ms;
    job->token = s->destroy_token;
    if (zcs_vod_thread_pool_run(pol, rtmp_vod_lane_open_blocking, rtmp_vod_lane_open_on_io, job) !=
        ZN_OK) {
        free(job);
        return 0;
    }
    return 1;
}

int rtmp_srv_onpause(void *param, int pause, uint32_t ms)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)param;
    uint32_t seek_ms = 0;

    if (!s || !s->source || !zcs_media_source_is_vod(s->source)) {
        return 0;
    }
    seek_ms = rtmp_netstream_ms_arg((double)ms, s->source);
    zn_info("[rtmp] pause session=%u pause=%d ms=%u", s->session_no, pause, (unsigned)seek_ms);
    if (pause) {
        if (seek_ms > 0) {
            zcs_rtmp_session_vod_do_seek(s, seek_ms, 0);
        } else {
            zcs_rtmp_session_vod_save_pause_pos(s);
        }
        zcs_egress_clock_pause(&s->pl.clk);
        zcs_rtmp_session_vod_pump_hold(s, 1);
    } else {
        if (seek_ms > 0) {
            zcs_rtmp_session_vod_do_seek(s, seek_ms, 0);
        } else if (s->pl.vod_pause_pos_valid) {
            zcs_rtmp_session_vod_do_seek(s, s->pl.vod_pause_ms, 0);
            s->pl.vod_pause_pos_valid = 0;
        }
        zcs_rtmp_session_vod_resume_after_seek(s);
        s->pl.vod_catchup = (int)ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    }
    if (!pause && s->state == ZCS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending) {
        zcs_rtmp_session_play_flush_nolock(s);
    }
    return 0;
}

int rtmp_srv_onplayctrl(void *param, double speed)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)param;

    if (!s || !s->source || !zcs_media_source_is_vod(s->source)) {
        return 0;
    }
    zcs_rtmp_session_vod_set_play_scale(s, speed);
    s->pl.vod_catchup = (int)ZCS_RTMP_VOD_PLAY_FRAME_BUDGET_SEEK;
    if (s->state == ZCS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending) {
        zcs_rtmp_session_play_flush_nolock(s);
    }
    return 0;
}

int rtmp_srv_onseek(void *param, uint32_t ms)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)param;
    uint32_t seek_ms;

    if (!s) {
        return 0;
    }
    seek_ms = rtmp_netstream_ms_arg((double)ms, s->source);
    zn_info("[rtmp] seek_request session=%u raw_ms=%u seek_ms=%u", s->session_no, (unsigned)ms,
             (unsigned)seek_ms);
    if (zcs_egress_clock_is_paused(&s->pl.clk)) {
        zcs_egress_clock_resume(&s->pl.clk);
    }
    /* 媒体必须在发出 Seek.Notify/Play.Start 之后再推；flush 留给 on_recv */
    zcs_rtmp_session_vod_do_seek(s, seek_ms, 0);
    return 0;
}

int rtmp_srv_ongetduration(void *param, const char *app_name, const char *stream_name,
                           double *duration)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)param;
    zcs_media_source *src;
    uint64_t dur_ms = 0;

    if (!duration) {
        return 0;
    }
    *duration = 0.0;
    src = s && s->source ? s->source : NULL;
    if (!src && app_name && stream_name) {
        src = zcs_media_source_find_api_ex(ZCS_SCHEMA_RTMP, s->vhost, app_name, stream_name);
    }
    if (src && zcs_media_source_is_vod(src)) {
        dur_ms = zcs_vod_source_duration_ms(src);
    }
    if (!dur_ms && app_name && stream_name) {
        dur_ms = zcs_vod_probe_duration_ms(app_name, stream_name);
    }
    if (dur_ms > 0) {
        *duration = dur_ms / 1000.0;
    }
    if (*duration > 0.0) {
        zn_debug("[rtmp] get_stream_length session=%u sec=%.3f", s ? s->session_no : 0, *duration);
    }
    return 0;
}
