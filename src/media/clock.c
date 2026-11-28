/**
 * @file clock.c
 * @brief 入站 DeltaStamp、media_timeline 与出站时钟辅助。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/clock.h"
#include "zcs/media/media_limits.h"
#include "zn/util/timer.h"
#include <string.h>

#define ZCS_MPEGTS_CLOCK_HZ 90000u
#define ZCS_NTP_UNIX_OFFSET 2208988800ULL
#define ZCS_STAMP_MAX_DELTA_MS 300u
#define ZCS_STAMP_SYNC_DRIFT_MS 300
#define ZCS_STAMP_SYNC_INPUT_MS 5000

static int64_t i64_abs(int64_t v)
{
    return v >= 0 ? v : -v;
}

void zcs_track_stamp_reset(zcs_track_stamp *s)
{
    if (!s) {
        return;
    }
    uint32_t max_delta = s->max_delta_ms > 0 ? s->max_delta_ms : ZCS_STAMP_MAX_DELTA_MS;
    zcs_track_stamp *master = s->sync_master;
    memset(s, 0, sizeof(*s));
    s->max_delta_ms = max_delta;
    s->sync_master = master;
    s->last_delta = 1;
}

void zcs_track_stamp_sync_to(zcs_track_stamp *s, zcs_track_stamp *master, int count)
{
    if (!s || !master) {
        return;
    }
    s->sync_master = master;
    s->need_sync += count > 0 ? count : 1;
}

static int64_t stamp_delta(zcs_track_stamp *s, int64_t stamp)
{
    if (!s->last_stamp) {
        if (stamp) {
            s->last_stamp = stamp;
        }
        return 0;
    }

    int64_t ret = stamp - s->last_stamp;
    if (ret >= 0) {
        s->last_stamp = stamp;
        if (ret > (int64_t)s->max_delta_ms) {
            s->need_sync++;
            return s->last_delta > 0 ? s->last_delta : 1;
        }
        s->last_delta = ret > 0 ? ret : 1;
        return ret;
    }

    s->last_stamp = stamp;
    if (-ret > (int64_t)s->max_delta_ms) {
        s->need_sync++;
        return s->last_delta > 0 ? s->last_delta : 1;
    }
    return ret;
}

static void stamp_sync_l(zcs_track_stamp *s)
{
    zcs_track_stamp *m = s->sync_master;
    if (!m || !m->last_dts_in) {
        return;
    }
    if (!s->need_sync && !m->need_sync) {
        return;
    }

    int64_t in_diff = s->last_dts_in - m->last_dts_in;
    if (i64_abs(in_diff) >= ZCS_STAMP_SYNC_INPUT_MS && s->need_sync <= 3) {
        return;
    }

    int64_t rel_diff = s->relative_ms - m->relative_ms;
    int64_t dts_diff = rel_diff;
    if (dts_diff > ZCS_STAMP_SYNC_DRIFT_MS) {
        dts_diff = 0;
    } else if (dts_diff < -ZCS_STAMP_SYNC_DRIFT_MS) {
        dts_diff = 0;
    }

    int64_t target = m->relative_ms + dts_diff;
    if (target > s->relative_ms) {
        s->relative_ms = target;
        s->last_stamp = s->last_dts_in;
    } else if (target < m->relative_ms) {
        m->relative_ms = target;
        m->last_stamp = m->last_dts_in;
    }

    if (s->need_sync > 0) {
        s->need_sync--;
    }
    if (m->need_sync > 0) {
        m->need_sync--;
    }
}

uint32_t zcs_track_stamp_revise(zcs_track_stamp *s, uint64_t raw_ms)
{
    if (!s) {
        return (uint32_t)raw_ms;
    }
    if (!s->max_delta_ms) {
        s->max_delta_ms = ZCS_STAMP_MAX_DELTA_MS;
    }

    int64_t dts = (int64_t)raw_ms;
    if (s->last_dts_in != dts) {
        s->relative_ms += stamp_delta(s, dts);
        s->last_dts_in = dts;
    }

    stamp_sync_l(s);
    if (s->relative_ms < 0) {
        return 0;
    }
    return (uint32_t)s->relative_ms;
}

void zcs_media_timeline_link_stamps(zcs_media_timeline *tl)
{
    if (!tl || tl->stamp_linked) {
        return;
    }
    zcs_track_stamp_sync_to(&tl->ast, &tl->vst, 1);
    tl->stamp_linked = 1;
}

void zcs_media_timeline_set_av_clamp(zcs_media_timeline *tl, int enabled)
{
    if (!tl) {
        return;
    }
    tl->av_clamp_disabled = enabled ? 0 : 1;
}

zcs_time_base zcs_time_base_ms(void)
{
    zcs_time_base tb = {1, 1000};
    return tb;
}

zcs_time_base zcs_time_base_from_hz(uint32_t clock_hz)
{
    zcs_time_base tb = {1, clock_hz > 0 ? clock_hz : ZCS_MPEGTS_CLOCK_HZ};
    return tb;
}

uint64_t zcs_ts_rescale_q(uint64_t ts, zcs_time_base from_tb, zcs_time_base to_tb)
{
    if (!from_tb.den || !to_tb.den) {
        return ts;
    }
    return (ts * (uint64_t)to_tb.num * (uint64_t)from_tb.den) /
           ((uint64_t)to_tb.den * (uint64_t)from_tb.num);
}

uint32_t zcs_rtp_clock_to_ms(uint32_t rtp_ts, uint32_t clock_hz)
{
    uint32_t hz = clock_hz > 0 ? clock_hz : ZCS_MPEGTS_CLOCK_HZ;
    return (uint32_t)((uint64_t)rtp_ts * 1000u / (uint64_t)hz);
}

uint32_t zcs_ms_to_rtp_clock(uint32_t ms, uint32_t clock_hz)
{
    uint32_t hz = clock_hz > 0 ? clock_hz : ZCS_MPEGTS_CLOCK_HZ;
    return (uint32_t)((uint64_t)ms * (uint64_t)hz / 1000u);
}

uint64_t zcs_ms_to_mpegts_90k(uint32_t ms)
{
    return (uint64_t)ms * ZCS_MPEGTS_CLOCK_HZ / 1000u;
}

uint32_t zcs_mpegts_90k_to_ms(int64_t ticks_90k)
{
    if (ticks_90k < 0) {
        return 0;
    }
    return (uint32_t)(ticks_90k / 90);
}

uint64_t zcs_wall_ms(void)
{
    return zn_wall_ms();
}

void zcs_wall_ms_to_ntp(uint64_t wall_ms, uint32_t *ntp_sec, uint32_t *ntp_frac)
{
    if (ntp_sec) {
        *ntp_sec = (uint32_t)(wall_ms / 1000u + ZCS_NTP_UNIX_OFFSET);
    }
    if (ntp_frac) {
        uint32_t ms = (uint32_t)(wall_ms % 1000u);
        *ntp_frac = (uint32_t)((uint64_t)ms * 0x100000000ULL / 1000u);
    }
}

uint32_t zcs_codec_frame_duration_ms(zcs_codec_id codec, uint32_t sample_rate_hz)
{
    switch (codec) {
    case ZCS_CODEC_AAC: {
        uint32_t rate = sample_rate_hz > 0 ? sample_rate_hz : 44100;
        return (1024u * 1000u + rate / 2) / rate;
    }
    case ZCS_CODEC_H264:
        return 40u;
    default:
        return ZCS_MEDIA_TS_MIN_STEP_MS;
    }
}

void zcs_stream_clock_reset(zcs_stream_clock *c)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
}

void zcs_stream_clock_init(zcs_stream_clock *c, zcs_track_type track, zcs_codec_id codec,
                           uint32_t sample_rate_hz)
{
    if (!c) {
        return;
    }
    c->track = track;
    c->frame_dur_ms = zcs_codec_frame_duration_ms(codec, sample_rate_hz);
    if (track == ZCS_TRACK_VIDEO && c->frame_dur_ms < 40) {
        c->frame_dur_ms = 40;
    }
    c->learned_delta_ms = (track == ZCS_TRACK_VIDEO) ? 133u : c->frame_dur_ms;
}

static uint32_t stream_clock_step(const zcs_stream_clock *c)
{
    if (c->track == ZCS_TRACK_VIDEO && c->learned_delta_ms >= 16 && c->learned_delta_ms <= 500) {
        return c->learned_delta_ms;
    }
    return c->frame_dur_ms > 0 ? c->frame_dur_ms : ZCS_MEDIA_TS_MIN_STEP_MS;
}

uint32_t zcs_stream_clock_advance(zcs_stream_clock *c, uint64_t raw_ms)
{
    if (!c) {
        return (uint32_t)raw_ms;
    }

    if (!c->started) {
        c->started = 1;
        c->last_raw_ms = raw_ms;
        c->last_pts_ms = 0;
        return 0;
    }

    if (c->last_pts_ms > ZCS_MEDIA_TS_REWIND_THRESHOLD_MS &&
        raw_ms + ZCS_MEDIA_TS_REWIND_THRESHOLD_MS < c->last_raw_ms) {
        c->last_pts_ms += stream_clock_step(c);
        c->last_raw_ms = raw_ms;
        return c->last_pts_ms;
    }

    if (raw_ms > c->last_raw_ms) {
        uint32_t rel = (uint32_t)(raw_ms - c->last_raw_ms);
        if (c->track == ZCS_TRACK_VIDEO && rel >= 16 && rel <= 500) {
            c->learned_delta_ms = rel;
        }
        c->last_pts_ms += rel;
    } else if (raw_ms == c->last_raw_ms) {
        return c->last_pts_ms;
    } else {
        c->last_pts_ms += stream_clock_step(c);
    }

    c->last_raw_ms = raw_ms;
    return c->last_pts_ms;
}

void zcs_media_timeline_reset(zcs_media_timeline *tl)
{
    if (!tl) {
        return;
    }
    memset(tl, 0, sizeof(*tl));
    tl->video_clock_hz = ZCS_MPEGTS_CLOCK_HZ;
    tl->audio_clock_hz = 44100;
    tl->vst.max_delta_ms = ZCS_STAMP_MAX_DELTA_MS;
    tl->ast.max_delta_ms = ZCS_STAMP_MAX_DELTA_MS;
    zcs_stream_clock_init(&tl->video, ZCS_TRACK_VIDEO, ZCS_CODEC_H264, 0);
    zcs_stream_clock_init(&tl->audio, ZCS_TRACK_AUDIO, ZCS_CODEC_AAC, tl->audio_clock_hz);
}

void zcs_media_timeline_set_video(zcs_media_timeline *tl, zcs_codec_id codec, uint32_t clock_hz)
{
    if (!tl) {
        return;
    }
    tl->video_clock_hz = clock_hz > 0 ? clock_hz : ZCS_MPEGTS_CLOCK_HZ;
    zcs_stream_clock_init(&tl->video, ZCS_TRACK_VIDEO, codec, 0);
}

void zcs_media_timeline_set_audio(zcs_media_timeline *tl, zcs_codec_id codec, uint32_t clock_hz)
{
    if (!tl) {
        return;
    }
    tl->audio_codec = codec;
    tl->audio_clock_hz = clock_hz > 0 ? clock_hz : 44100;
    zcs_stream_clock_init(&tl->audio, ZCS_TRACK_AUDIO, codec, tl->audio_clock_hz);
    zcs_media_timeline_link_stamps(tl);
}

static void stamp_clamp_av(zcs_media_timeline *tl)
{
    if (!tl || tl->av_clamp_disabled || !tl->stamp_linked || !tl->ast.last_dts_in ||
        !tl->vst.last_dts_in) {
        return;
    }
    int64_t diff = tl->vst.relative_ms - tl->ast.relative_ms;
    if (diff > ZCS_STAMP_SYNC_DRIFT_MS) {
        tl->vst.relative_ms = tl->ast.relative_ms;
        tl->vst.last_stamp = tl->vst.last_dts_in;
    } else if (diff < -ZCS_STAMP_SYNC_DRIFT_MS) {
        tl->ast.relative_ms = tl->vst.relative_ms;
        tl->ast.last_stamp = tl->ast.last_dts_in;
    }
}

uint32_t zcs_media_timeline_video(zcs_media_timeline *tl, uint64_t raw_ms)
{
    if (!tl) {
        return (uint32_t)raw_ms;
    }
    zcs_media_timeline_link_stamps(tl);
    (void)zcs_track_stamp_revise(&tl->vst, raw_ms);
    stamp_clamp_av(tl);
    return (uint32_t)tl->vst.relative_ms;
}

uint32_t zcs_media_timeline_audio(zcs_media_timeline *tl, uint64_t raw_ms)
{
    if (!tl) {
        return (uint32_t)raw_ms;
    }
    zcs_media_timeline_link_stamps(tl);
    (void)zcs_track_stamp_revise(&tl->ast, raw_ms);
    stamp_clamp_av(tl);
    return (uint32_t)tl->ast.relative_ms;
}

void zcs_mux_timeline_reset(zcs_mux_timeline *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

uint32_t zcs_mux_timeline_pts(zcs_mux_timeline *m, uint32_t ring_pts_ms)
{
    if (!m) {
        return ring_pts_ms;
    }

    if (!m->origin_set) {
        m->origin_ms = ring_pts_ms;
        m->origin_set = 1;
        m->last_out_ms = 0;
        return 0;
    }

    uint32_t out = ring_pts_ms >= m->origin_ms ? ring_pts_ms - m->origin_ms : 0;
    if (out <= m->last_out_ms) {
        out = m->last_out_ms + 1u;
    }
    m->last_out_ms = out;
    return out;
}

void zcs_mux_av_timeline_reset(zcs_mux_av_timeline *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
}

void zcs_mux_av_timeline_lock_origin(zcs_mux_av_timeline *m, uint32_t origin_ms)
{
    if (!m) {
        return;
    }
    m->shared_origin_ms = origin_ms;
    m->use_shared_origin = 1;
    m->origin_ms[0] = m->origin_ms[1] = origin_ms;
    m->origin_set[0] = m->origin_set[1] = 1;
    m->last_out_ms[0] = m->last_out_ms[1] = 0;
}

void zcs_mux_av_timeline_shift_origin(zcs_mux_av_timeline *m, uint32_t origin_ms)
{
    if (!m) {
        return;
    }
    m->shared_origin_ms = origin_ms;
    m->use_shared_origin = 1;
    m->origin_ms[0] = m->origin_ms[1] = origin_ms;
    m->origin_set[0] = m->origin_set[1] = 1;
}

static uint32_t timeline_rel_ms(uint32_t ring_ms, uint32_t origin_ms)
{
    uint32_t rel = ring_ms >= origin_ms ? ring_ms - origin_ms : 0;

    /* B 帧的首个 DTS 可能小于锚点，ring 里以 uint32 保存时会回绕成接近
     * 4.29e9 的值；按 0 处理，否则 HLS/DASH 的段时长和 MPD SegmentTimeline
     * 会出现负数/巨值。 */
    if (rel > 0x7FFFFFFFu) {
        rel = 0;
    }
    return rel;
}

uint32_t zcs_mux_av_timeline_pts(zcs_mux_av_timeline *m, zcs_track_type track, uint32_t ring_pts_ms)
{
    unsigned idx = (track == ZCS_TRACK_AUDIO) ? 1u : 0u;
    if (!m) {
        return ring_pts_ms;
    }

    if (m->use_shared_origin) {
        uint32_t rel = timeline_rel_ms(ring_pts_ms, m->shared_origin_ms);
        /* 首帧样本允许 t=0；输出开始后才强制单调。 */
        if (m->last_out_ms[idx] > 0 && rel <= m->last_out_ms[idx]) {
            rel = m->last_out_ms[idx] + 1u;
        }
        m->last_out_ms[idx] = rel;
        return rel;
    }

    if (!m->origin_set[idx]) {
        m->origin_ms[idx] = ring_pts_ms;
        m->origin_set[idx] = 1;
    }

    uint32_t rel = timeline_rel_ms(ring_pts_ms, m->origin_ms[idx]);
    /* 仅按 track 单调（VLC FLV：A/V tag 可共享文件顺序，非单一全局 DTS）。 */
    if (rel <= m->last_out_ms[idx]) {
        rel = m->last_out_ms[idx] + 1u;
    }
    m->last_out_ms[idx] = rel;
    return rel;
}
