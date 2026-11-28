#ifndef ZCS_EGRESS_PACING_EGRESS_PACING_H
#define ZCS_EGRESS_PACING_EGRESS_PACING_H

/**
 * @file egress_pacing.h
 * @brief 进程级出站 pacing 配置。
 *
 * 进程启动时经 @ref zcs_egress_pacing_init() 初始化一次；所有读者经宏访问 const 指针（零开销内联）。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_egress_pacing {
    uint32_t ring_max_lag;
    uint32_t catchup_lag;
    int frame_budget_live;
    int frame_budget_catchup;
    int burst_budget;
    int flv_budget_live;
    int flv_budget_vod;
    int vod_catchup_start;
    int vod_catchup_seek;
    uint32_t vod_prefill_lag;
    uint32_t resync_lag;
    uint32_t resync_lag_max;
    uint32_t resync_cooldown_ms;
    uint32_t pace_lead_ms;
    uint32_t slow_consumer_kick_lag;
} zcs_egress_pacing;

/** 用内置默认值填充全字段（直播/点播的帧预算、追赶阈值等），不校验。 */
ZCS_API void zcs_egress_pacing_defaults(zcs_egress_pacing *p);
/**
 * 初始化进程级 pacing 运行时配置（启动时调用一次）。
 * @param cfg 配置来源；NULL 时用 defaults。内部会做下限裁剪（如 ring_max_lag>=32）。
 */
ZCS_API void zcs_egress_pacing_init(const zcs_egress_pacing *cfg);
/** 清空 pacing 配置并标记未初始化（下次 get 会以默认值重建）。 */
ZCS_API void zcs_egress_pacing_fini(void);
/** 取进程级配置只读指针；未初始化时自动以默认值初始化（幂等）。 */
ZCS_API const zcs_egress_pacing *zcs_egress_pacing_get(void);

#define ZCS_EGRESS_RING_MAX_LAG (zcs_egress_pacing_get()->ring_max_lag)
#define ZCS_EGRESS_CATCHUP_LAG (zcs_egress_pacing_get()->catchup_lag)
#define ZCS_EGRESS_FRAME_BUDGET_LIVE (zcs_egress_pacing_get()->frame_budget_live)
#define ZCS_EGRESS_FRAME_BUDGET_CATCHUP (zcs_egress_pacing_get()->frame_budget_catchup)
#define ZCS_EGRESS_BURST_BUDGET (zcs_egress_pacing_get()->burst_budget)
#define ZCS_EGRESS_FLV_BUDGET_LIVE (zcs_egress_pacing_get()->flv_budget_live)
#define ZCS_EGRESS_FLV_BUDGET_VOD (zcs_egress_pacing_get()->flv_budget_vod)
#define ZCS_EGRESS_VOD_CATCHUP_START (zcs_egress_pacing_get()->vod_catchup_start)
#define ZCS_EGRESS_VOD_CATCHUP_SEEK (zcs_egress_pacing_get()->vod_catchup_seek)
#define ZCS_EGRESS_VOD_PREFILL_LAG (zcs_egress_pacing_get()->vod_prefill_lag)
#define ZCS_EGRESS_RESYNC_LAG (zcs_egress_pacing_get()->resync_lag)
#define ZCS_EGRESS_RESYNC_LAG_MAX (zcs_egress_pacing_get()->resync_lag_max)
#define ZCS_EGRESS_RESYNC_COOLDOWN_MS (zcs_egress_pacing_get()->resync_cooldown_ms)
#define ZCS_EGRESS_PACE_LEAD_MS (zcs_egress_pacing_get()->pace_lead_ms)
#define ZCS_EGRESS_SLOW_CONSUMER_KICK_LAG (zcs_egress_pacing_get()->slow_consumer_kick_lag)

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_PACING_EGRESS_PACING_H */
