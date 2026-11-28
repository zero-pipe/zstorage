/**
 * @file stats.c
 * @brief 每路流的入/出站字节计数、fps 估计与丢帧计数。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/stream/stats.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/zcs_atomic.h"
#include "zn/platform.h"
#include <string.h>

#define ZCS_STATS_SPEED_INTERVAL_MS 1000u

static void stats_refresh_speed(zcs_media_stream_stats *st, uint64_t now)
{
    uint64_t dt;

    if (!st || st->speed_tick_ms == 0) {
        if (st) {
            st->speed_tick_ms = now;
            st->ingress_at_tick = st->ingress_bytes;
            st->egress_at_tick = st->egress_bytes;
        }
        return;
    }
    if (now <= st->speed_tick_ms) {
        return;
    }
    dt = now - st->speed_tick_ms;
    if (dt < ZCS_STATS_SPEED_INTERVAL_MS) {
        return;
    }
    st->ingress_speed_bps = (st->ingress_bytes - st->ingress_at_tick) * 1000ull / dt;
    st->egress_speed_bps = (st->egress_bytes - st->egress_at_tick) * 1000ull / dt;
    st->speed_tick_ms = now;
    st->ingress_at_tick = st->ingress_bytes;
    st->egress_at_tick = st->egress_bytes;
}

static void stats_refresh_fps(zcs_media_stream_stats *st, uint64_t now)
{
    uint64_t dt;

    if (!st) {
        return;
    }
    if (st->fps_tick_ms == 0) {
        st->fps_tick_ms = now;
        st->fps_v_at_tick = st->frame_count_v;
        st->fps_a_at_tick = st->frame_count_a;
        return;
    }
    if (now <= st->fps_tick_ms) {
        return;
    }
    dt = now - st->fps_tick_ms;
    if (dt < ZCS_STATS_SPEED_INTERVAL_MS) {
        return;
    }
    /* frames/s = delta_frames * 1000 / dt_ms */
    st->video_fps = (uint32_t)((st->frame_count_v - st->fps_v_at_tick) * 1000ull / dt);
    st->audio_fps = (uint32_t)((st->frame_count_a - st->fps_a_at_tick) * 1000ull / dt);
    st->fps_tick_ms = now;
    st->fps_v_at_tick = st->frame_count_v;
    st->fps_a_at_tick = st->frame_count_a;
}

static void stats_add_bytes(zcs_media_stream_stats *st, uint64_t *total, size_t nbytes)
{
    uint64_t now;

    if (!st || !total || nbytes == 0) {
        return;
    }
    *total += (uint64_t)nbytes;
    now = zn_monotonic_ms();
    if (st->speed_tick_ms == 0) {
        st->speed_tick_ms = now;
        st->ingress_at_tick = st->ingress_bytes;
        st->egress_at_tick = st->egress_bytes;
        return;
    }
    stats_refresh_speed(st, now);
}

void zcs_media_stats_on_ingress(zcs_media_source *s, size_t nbytes)
{
    if (!s || nbytes == 0) {
        return;
    }
    stats_add_bytes(&s->stats, &s->stats.ingress_bytes, nbytes);
}

void zcs_media_stats_on_egress(zcs_media_source *s, size_t nbytes)
{
    if (!s || nbytes == 0) {
        return;
    }
    stats_add_bytes(&s->stats, &s->stats.egress_bytes, nbytes);
}

void zcs_media_stats_on_frame(zcs_media_source *s, int is_video)
{
    uint64_t now;

    if (!s) {
        return;
    }
    if (is_video) {
        ZCS_ATOMIC_ADD64(&s->stats.frame_count_v, 1);
    } else {
        ZCS_ATOMIC_ADD64(&s->stats.frame_count_a, 1);
    }
    now = zn_monotonic_ms();
    stats_refresh_fps(&s->stats, now);
}

void zcs_media_stats_on_drop(zcs_media_source *s)
{
    if (!s) {
        return;
    }
    ZCS_ATOMIC_ADD64(&s->stats.dropped_frames, 1);
}

void zcs_media_stats_reset(zcs_media_source *s)
{
    if (!s) {
        return;
    }
    memset(&s->stats, 0, sizeof(s->stats));
}

void zcs_media_stats_fill(const zcs_media_source *s, zcs_gop_queue *ring, zcs_media_stats_view *out)
{
    zcs_media_stream_stats snap;
    uint64_t now;

    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s) {
        return;
    }

    /* 原子快照三个热计数器，再拷贝其余字段。 */
    snap = s->stats;
    snap.frame_count_v = ZCS_ATOMIC_LOAD64(&s->stats.frame_count_v);
    snap.frame_count_a = ZCS_ATOMIC_LOAD64(&s->stats.frame_count_a);
    snap.dropped_frames = ZCS_ATOMIC_LOAD64(&s->stats.dropped_frames);
    now = zn_monotonic_ms();
    stats_refresh_speed(&snap, now);
    stats_refresh_fps(&snap, now);

    out->ingress_bytes = snap.ingress_bytes;
    out->egress_bytes = snap.egress_bytes;
    out->bytes_speed = (int64_t)snap.ingress_speed_bps;
    out->egress_speed = (int64_t)snap.egress_speed_bps;
    out->video_fps = snap.video_fps;
    out->audio_fps = snap.audio_fps;
    out->dropped_frames = snap.dropped_frames;

    if (ring) {
        out->gop_queue_pending = zcs_gop_queue_pending_count(ring);
        out->gop_queue_max_lag = zcs_gop_queue_max_reader_lag(ring);
        out->gop_queue_gop_count = zcs_gop_queue_gop_count(ring);
        out->gop_queue_readers = zcs_gop_reader_count(ring);
    }
}
