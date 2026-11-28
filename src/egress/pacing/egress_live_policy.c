/**
 * @file egress_live_policy.c
 * @brief 直播出站 pacing / snap / catchup 策略。
 */
#include "zcs/egress/pacing/egress_live_policy.h"
#include "zcs/media/clock.h"
#include "zcs/core/gop/queue.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include "zn/util/log.h"

int zcs_egress_live_pump_budget(int epoch_on, int catchup_done, size_t lag, int caller_budget)
{
    int budget = caller_budget > 0 ? caller_budget : (int)ZCS_EGRESS_FRAME_BUDGET_LIVE;

    if (zcs_egress_live_lag_catchup(lag) || !epoch_on || !catchup_done) {
        budget = (int)ZCS_EGRESS_BURST_BUDGET;
    }
    if (epoch_on && catchup_done && budget > (int)ZCS_EGRESS_FRAME_BUDGET_LIVE) {
        budget = (int)ZCS_EGRESS_FRAME_BUDGET_LIVE;
    }
    return budget;
}

int zcs_egress_live_peek_paced(zcs_gop_reader *rd, zcs_gop_slot *peek, const zcs_egress_clock *clk,
                               int pace, int read_timeout_ms)
{
    if (!rd || !peek) {
        return -1;
    }
    if (zcs_gop_reader_peek_muxed(rd, peek, read_timeout_ms) <= 0) {
        return -1;
    }
    if (pace && clk && zcs_egress_clock_epoch_locked(clk) &&
        !zcs_egress_clock_media_due(clk, peek->dts_ms, ZCS_EGRESS_PACE_LEAD_MS)) {
        return 0;
    }
    return 1;
}

void zcs_egress_live_snap_gop(zcs_gop_reader *rd, struct zcs_rtp_muxer *mux)
{
    uint32_t anchor = 0;
    zcs_gop_slot at;

    if (rd) {
        zcs_gop_reader_seek_live(rd);
        if (zcs_gop_reader_slot_at_read(rd, &at) && at.track == ZCS_TRACK_VIDEO) {
            anchor = at.dts_ms;
        }
    }
    if (mux) {
        zcs_egress_clock *clk = zcs_rtp_muxer_play_clock_mut(mux);
        if (clk && zcs_egress_clock_epoch_locked(clk)) {
            zcs_egress_clock_sync_wall(clk, anchor);
        } else {
            zcs_rtp_muxer_jump_live(mux);
        }
    }
}

int zcs_egress_live_should_pump(zcs_gop_reader *rd, int woke, int catchup_done, int epoch_on)
{
    size_t lag;

    if (!rd) {
        return 0;
    }
    if (woke) {
        return 1;
    }
    if (!catchup_done || !epoch_on) {
        return 1;
    }
    lag = zcs_gop_reader_lag(rd);
    return lag > 0;
}

static const zcs_egress_clock *live_play_clock(zcs_rtp_muxer *mux,
                                               const zcs_egress_live_state *live)
{
    if (mux) {
        return zcs_rtp_muxer_play_clock(mux);
    }
    if (live && live->play_clk) {
        return live->play_clk;
    }
    return NULL;
}

void zcs_egress_live_seek_rtsp_attach(zcs_gop_reader *rd)
{
    if (rd) {
        zcs_gop_reader_seek_gop_key(rd);
    }
}

static int live_play_maybe_kick_slow_consumer(zcs_gop_reader *rd, const zcs_egress_live_state *live,
                                              int catchup_done, int epoch_on, size_t lag,
                                              unsigned session_no)
{
    unsigned kick_lag;
    void *user;

    kick_lag = ZCS_EGRESS_SLOW_CONSUMER_KICK_LAG;
    if (kick_lag == 0 || !catchup_done || !epoch_on || lag <= kick_lag) {
        return 0;
    }
    if (!live || !live->on_slow_consumer) {
        return 0;
    }
    user = live->slow_consumer_user ? live->slow_consumer_user : live->snap_user;
    if (session_no) {
        zn_warn("[live_play] reader_kick lag=%zu kick_lag=%u session=%u", lag, kick_lag,
                 session_no);
    }
    live->on_slow_consumer(user);
    return 1;
}

int zcs_egress_live_rtsp_prep(zcs_gop_reader *rd, zcs_rtp_muxer *mux,
                              const zcs_egress_live_state *live, unsigned session_no,
                              int *pump_budget_io)
{
    size_t lag;
    const zcs_egress_clock *clk;
    int catchup_done;
    int epoch_on;

    if (!rd) {
        return 0;
    }

    lag = zcs_gop_reader_lag(rd);
    clk = live_play_clock(mux, live);
    catchup_done = live && live->live_catchup_done && *live->live_catchup_done;
    epoch_on = clk && zcs_egress_clock_epoch_locked(clk);

    /* 首屏 catchup 期间跳过 snap：on_snap 会清 armed/config 并在 IDR 卡住。 */
    if (lag > ZCS_EGRESS_RING_MAX_LAG && (catchup_done || epoch_on)) {
        zcs_egress_live_snap_gop(rd, mux);
        if (live && live->on_snap) {
            live->on_snap(live->snap_user);
        } else if (session_no) {
            zn_debug("[live_play] snap_gop lag=%zu session=%u", zcs_gop_reader_lag(rd),
                      session_no);
        }
        lag = zcs_gop_reader_lag(rd);
        clk = live_play_clock(mux, live);
        catchup_done = live && live->live_catchup_done && *live->live_catchup_done;
        epoch_on = clk && zcs_egress_clock_epoch_locked(clk);
    }

    if (live && live->live_catchup_done) {
        if (!catchup_done && epoch_on && lag <= ZCS_EGRESS_CATCHUP_LAG) {
            *live->live_catchup_done = 1;
            catchup_done = 1;
        } else if (catchup_done && epoch_on &&
                   lag > ZCS_EGRESS_RING_MAX_LAG + ZCS_EGRESS_RESYNC_LAG) {
            uint64_t now = zcs_wall_ms();
            uint64_t last = live->live_resync_at_ms ? *live->live_resync_at_ms : 0;

            if (now - last >= ZCS_EGRESS_RESYNC_COOLDOWN_MS) {
                zcs_egress_live_snap_gop(rd, mux);
                if (live && live->on_snap) {
                    live->on_snap(live->snap_user);
                }
                if (live->live_resync_at_ms) {
                    *live->live_resync_at_ms = now;
                }
                epoch_on = clk && zcs_egress_clock_epoch_locked(clk);
                lag = zcs_gop_reader_lag(rd);
                if (session_no) {
                    zn_warn("[live_play] lag_resync lag=%zu session=%u", lag, session_no);
                }
            }
        } else if (!epoch_on && lag > ZCS_EGRESS_RING_MAX_LAG) {
            zcs_gop_reader_seek_live(rd);
            lag = zcs_gop_reader_lag(rd);
        }
    } else if (lag > ZCS_EGRESS_RING_MAX_LAG) {
        zcs_gop_reader_seek_live(rd);
    }

    if (pump_budget_io) {
        *pump_budget_io = zcs_egress_live_pump_budget(epoch_on, catchup_done, lag, *pump_budget_io);
    }
    if (pump_budget_io && live && live->pump_budget_cap > 0 &&
        *pump_budget_io > live->pump_budget_cap) {
        *pump_budget_io = live->pump_budget_cap;
    }

    return live_play_maybe_kick_slow_consumer(rd, live, catchup_done, epoch_on, lag, session_no);
}

int zcs_egress_live_flv_prep(zcs_gop_reader *rd, const zcs_egress_live_state *live,
                             unsigned session_no, int *pump_budget_io)
{
    return zcs_egress_live_rtsp_prep(rd, NULL, live, session_no, pump_budget_io);
}
