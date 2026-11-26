#ifndef ZMS_EGRESS_PACING_EGRESS_CLOCK_H
#define ZMS_EGRESS_PACING_EGRESS_CLOCK_H

/**
 * @file egress_clock.h
 * @brief 共享播放时钟：A/V epoch、实时 pacing、RTP/RTCP 时间戳辅助。
 *
 * GOP 缓存 / VOD fifo 提供归一化毫秒；时钟映射为各轨 RTP 时间戳与 NTP（RFC 3550）。
 * 供 RTP muxer、FLV/RTMP 播放与 egress pacing 使用。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZMS_EGRESS_PACE_LEAD_MS
#include "zms/egress/pacing/egress_pacing.h"
#endif

typedef struct zms_egress_clock {
    uint32_t epoch_ms;
    int epoch_locked;
    uint32_t epoch_ntp_sec;
    uint32_t epoch_ntp_frac;
    /** epoch 锁定时，墙钟毫秒使 RTP rel_ms 与实时播放对齐。 */
    uint64_t play_wall_ms;
    /** 直播：RTP ts = 媒体 dts_ms；SR NTP 须减去 epoch_ms。 */
    int abs_rtp_ts;
    int paused;
    uint64_t pause_wall_ms;
    double play_scale;
} zms_egress_clock;

/** 初始化为未锁定、未暂停、scale=1 的时钟；对 NULL 安全。 */
ZMS_API void zms_egress_clock_init(zms_egress_clock *c);
/** 重置（等价于 init），用于复用同一时钟结构重新开始播放。 */
ZMS_API void zms_egress_clock_reset(zms_egress_clock *c);

/** 捕获墙钟 NTP 作为流 epoch（PLAY 200 / muxer reset）。 */
ZMS_API void zms_egress_clock_arm(zms_egress_clock *c);

/** 在首个媒体 IDR 锁定 epoch；新锁定时返回 1。 */
ZMS_API int zms_egress_clock_lock_epoch(zms_egress_clock *c, uint32_t dts_ms);

/** 查询 epoch 是否已锁定（首个媒体帧之后为真）。 */
ZMS_API int zms_egress_clock_epoch_locked(const zms_egress_clock *c);

/** @return epoch 以来的 rel_ms；未锁或 dts 早于 epoch 则为 0。 */
ZMS_API uint32_t zms_egress_clock_rel_ms(const zms_egress_clock *c, uint32_t dts_ms);

/** 将相对毫秒按 clock_hz 换算为 RTP 时间戳（纯计算，不读写时钟状态）。 */
ZMS_API uint32_t zms_egress_clock_rtp_ts_rel(uint32_t rel_ms, uint32_t clock_hz);

/**
 * 取该 dts 对应的 RTP 时间戳（按 clock_hz 时钟频率）。
 * 直播（abs_rtp_ts）下直接返回媒体 dts 换算值；否则返回 epoch 以来的相对值。
 */
ZMS_API uint32_t zms_egress_clock_rtp_ts(const zms_egress_clock *c, uint32_t dts_ms,
                                         uint32_t clock_hz);

/** 与轨上 rtp_ts 匹配的 RTCP SR NTP。 */
ZMS_API void zms_egress_clock_sr_ntp(const zms_egress_clock *c, uint32_t rtp_ts, uint32_t clock_hz,
                                     uint32_t *ntp_sec, uint32_t *ntp_frac);

/**
 * @return 非零表示媒体时间已到期（rel_ms <= 已过墙钟 + lead_ms）。
 */
ZMS_API int zms_egress_clock_media_due(const zms_egress_clock *c, uint32_t dts_ms,
                                       uint32_t lead_ms);

/** 设置播放倍速（1.0 为实时，>1 加速追赶）；影响 media_due 的亚实时判断。 */
ZMS_API void zms_egress_clock_set_scale(zms_egress_clock *c, double scale);
/** 暂停：记录暂停墙钟，media_due 在暂停期间不再到期（PAUSE 命令）。 */
ZMS_API void zms_egress_clock_pause(zms_egress_clock *c);
/** 恢复：把暂停时长补回 play_wall，避免恢复后瞬间追赶。 */
ZMS_API void zms_egress_clock_resume(zms_egress_clock *c);
/** 查询当前是否处于暂停状态。 */
ZMS_API int zms_egress_clock_is_paused(const zms_egress_clock *c);

/** 清除锁定的 epoch，回到未锁定状态（重新起播/切流前调用）。 */
ZMS_API void zms_egress_clock_unlock(zms_egress_clock *c);

/** catchup 排空后：将 epoch 对齐到 anchor 并将 play_wall 重置为 now。 */
ZMS_API void zms_egress_clock_rebase(zms_egress_clock *c, uint32_t anchor_ms);

/** 直播 catchup 完成：仅同步 play_wall；epoch_ms 不变（RTP 保持单调）。 */
ZMS_API void zms_egress_clock_sync_wall(zms_egress_clock *c, uint32_t dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_PACING_EGRESS_CLOCK_H */
