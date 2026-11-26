/**
 * @file zs_clock.c
 * @brief 时间戳归一化实现。
 */
#include "storage/zs_clock.h"

#include <stdlib.h>
#include <string.h>

#define ZS_CLOCK_TS_HZ INT64_C(90)
#define ZS_PTS_WRAP (INT64_C(1) << 33)
#define ZS_PTS_HALF (INT64_C(1) << 32)
#define ZS_PTS_MASK (ZS_PTS_WRAP - 1)
#define ZS_DEFAULT_FRAME_MS 40u

struct zs_clock {
    uint32_t gap_ms;
    uint32_t resync_ms;
    int64_t last_raw;
    int64_t out_pts;
    int64_t utc_ms;
    uint32_t disc_count;
    int anchored;
};

static const zs_clock_cfg zs_clock_defaults = {2000u, 3000u};

/** 按 33 位回绕展开差值：结果在 [-2^32, 2^32)。 */
static int64_t zs_wrap_delta(int64_t raw, int64_t last)
{
    int64_t d = (raw - last) % ZS_PTS_WRAP;

    if (d < 0) {
        d += ZS_PTS_WRAP;
    }
    if (d >= ZS_PTS_HALF) {
        d -= ZS_PTS_WRAP;
    }
    return d;
}

zs_clock *zs_clock_create(const zs_clock_cfg *cfg)
{
    zs_clock *c = (zs_clock *)calloc(1, sizeof(*c));

    if (!c) {
        return NULL;
    }
    c->gap_ms = (cfg && cfg->gap_ms) ? cfg->gap_ms : zs_clock_defaults.gap_ms;
    c->resync_ms = (cfg && cfg->resync_ms) ? cfg->resync_ms : zs_clock_defaults.resync_ms;
    return c;
}

void zs_clock_destroy(zs_clock *c)
{
    free(c);
}

void zs_clock_reset(zs_clock *c)
{
    if (!c) {
        return;
    }
    c->anchored = 0;
    c->last_raw = 0;
    c->out_pts = 0;
    c->utc_ms = 0;
}

/* 单调前进到 wall，且至少前进 step。 */
static void zs_clock_advance_to_wall(zs_clock *c, int64_t wall_ms, int64_t step)
{
    c->out_pts += step;
    if (wall_ms * ZS_CLOCK_TS_HZ > c->out_pts) {
        c->out_pts = wall_ms * ZS_CLOCK_TS_HZ;
    }
    c->utc_ms = c->out_pts / ZS_CLOCK_TS_HZ;
}

void zs_clock_input(zs_clock *c, int64_t raw_pts_90k, int64_t wall_ms, uint32_t frame_dur_ms,
                    zs_clock_out *out)
{
    int64_t raw;
    int64_t delta;
    int64_t step;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!c) {
        return;
    }
    if (frame_dur_ms == 0) {
        frame_dur_ms = ZS_DEFAULT_FRAME_MS;
    }
    raw = raw_pts_90k & ZS_PTS_MASK;

    if (!c->anchored) {
        c->out_pts = wall_ms * ZS_CLOCK_TS_HZ;
        c->utc_ms = wall_ms;
        c->last_raw = raw;
        c->anchored = 1;
        c->disc_count++;
        if (out) {
            out->discontinuity = 1;
        }
        goto emit;
    }

    delta = zs_wrap_delta(raw, c->last_raw);
    step = (int64_t)frame_dur_ms * ZS_CLOCK_TS_HZ;

    if (delta <= 0 || delta > (int64_t)c->gap_ms * ZS_CLOCK_TS_HZ) {
        /* 断流 / 相机重启归零 / PTS 倒退：按帧长前进一步，并对齐墙钟 */
        zs_clock_advance_to_wall(c, wall_ms, step);
        c->last_raw = raw;
        c->disc_count++;
        if (out) {
            out->discontinuity = 1;
        }
        goto emit;
    }

    c->out_pts += delta;
    c->utc_ms = c->out_pts / ZS_CLOCK_TS_HZ;
    c->last_raw = raw;

    /* 相机时钟比墙钟慢太多（长期漂移/校时）时向前重锚；只允许向前，避免抖动 */
    if (wall_ms > c->utc_ms + (int64_t)c->resync_ms) {
        zs_clock_advance_to_wall(c, wall_ms, step);
        c->disc_count++;
        if (out) {
            out->discontinuity = 1;
        }
    }

emit:
    if (out) {
        out->pts_90k = c->out_pts;
        out->utc_ms = c->utc_ms;
        out->valid = 1;
    }
}

uint32_t zs_clock_discontinuity_count(const zs_clock *c)
{
    return c ? c->disc_count : 0u;
}
