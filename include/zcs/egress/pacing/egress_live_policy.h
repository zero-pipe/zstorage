#ifndef ZCS_EGRESS_PACING_EGRESS_LIVE_POLICY_H
#define ZCS_EGRESS_PACING_EGRESS_LIVE_POLICY_H

/**
 * @file egress_live_policy.h
 * @brief 直播出站 pacing：peek、catchup、snap/resync，供 RTSP/FLV/RTMP/WebRTC 泵共用。
 *
 * 阈值来自 egress_pacing.h。
 */
#include "zcs/core/gop/queue.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_rtp_muxer;

/** 可选直播泵会话钩子（catchup / snap / 慢消费者）。 */
typedef struct zcs_egress_live_state {
    int *live_catchup_done;
    uint64_t *live_resync_at_ms;
    /** 未绑定 RTP mux 时的播放时钟（FLV/RTMP）。 */
    zcs_egress_clock *play_clk;
    /** 滞后驱动 GOP snap 后调用（如重置 RTP 队列 / 重发 config）。 */
    void (*on_snap)(void *user);
    void *snap_user;
    /** 滞后超过 ZCS_EGRESS_SLOW_CONSUMER_KICK_LAG 时调用（0 表示禁用）。 */
    void (*on_slow_consumer)(void *user);
    void *slow_consumer_user;
    /** >0 时限制每 tick 帧数（如 WebRTC 小步发送）。 */
    int pump_budget_cap;
    /** 1 = 低延迟模式：不按播放时钟 pacing，读到即发（ZLM lowLatency）。 */
    int low_latency;
} zcs_egress_live_state;

static inline int zcs_egress_live_lag_catchup(size_t lag)
{
    return lag > ZCS_EGRESS_CATCHUP_LAG;
}

/** 空闲时跳过空直播读（无唤醒、无滞后）。 */
ZCS_API int zcs_egress_live_should_pump(zcs_gop_reader *rd, int woke, int catchup_done,
                                        int epoch_on);

/** 解析每 tick 帧预算（突发 vs 稳态）。 */
ZCS_API int zcs_egress_live_pump_budget(int epoch_on, int catchup_done, size_t lag,
                                        int caller_budget);

/**
 * 经 pacing 门控的下一 GOP 槽位 peek。
 * @return 1 可读，0 未到期/空，-1 错误
 */
ZCS_API int zcs_egress_live_peek_paced(zcs_gop_reader *rd, zcs_gop_slot *peek,
                                       const zcs_egress_clock *clk, int pace, int read_timeout_ms);

/**
 * RTSP 直播预备：snap / seek_live / resync / 预算上限。
 * @return 1 表示慢消费者 kick 已触发（停止 pump）
 */
ZCS_API int zcs_egress_live_rtsp_prep(zcs_gop_reader *rd, struct zcs_rtp_muxer *mux,
                                      const zcs_egress_live_state *live, unsigned session_no,
                                      int *pump_budget_io);

/** FLV/RTMP 直播预备：共享滞后 snap/resync；on_snap 在 IDR 前重臂 config。 */
ZCS_API int zcs_egress_live_flv_prep(zcs_gop_reader *rd, const zcs_egress_live_state *live,
                                     unsigned session_no, int *pump_budget_io);

/** 在 gop_queue 中 seek 直播关键帧并跳转 mux 时间线（bootstrap / snap）。 */
ZCS_API void zcs_egress_live_snap_gop(zcs_gop_reader *rd, struct zcs_rtp_muxer *mux);

/** 直播附着：seek 到最新 GOP 关键帧。 */
ZCS_API void zcs_egress_live_seek_rtsp_attach(zcs_gop_reader *rd);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_PACING_EGRESS_LIVE_POLICY_H */
