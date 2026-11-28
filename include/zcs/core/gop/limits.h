#ifndef ZCS_CORE_GOP_LIMITS_H
#define ZCS_CORE_GOP_LIMITS_H

/**
 * @file limits.h
 * @brief GOP 队列容量与读者策略常量。
 *
 * 常量与存储实现头文件解耦，便于 media/protocol 模块共享策略而不依赖 gop_queue.h。
 */

/** 每路直播源保留的 ring 槽位数。 */
#define ZCS_GOP_QUEUE_CAPACITY 512u

/** 单队列最多跟踪的 GOP 边界数。 */
#define ZCS_GOP_QUEUE_MAX_GOP 32u

/** 压力下丢弃旧 GOP 前的目标保留 GOP 数。
 *  运行时覆盖：[general] gop_target_gops / zcs_gop_queue_set_default_target_gops。 */
#define ZCS_GOP_QUEUE_TARGET_GOPS 3u

/**
 * 每路 GOP 缓存的默认字节上限。0 表示不限。
 * 1080p/8Mbps 下约等于 8 秒；与时间窗同时生效，先触者为准。
 */
#define ZCS_GOP_QUEUE_DEFAULT_BYTE_BUDGET (8u * 1024u * 1024u)

/**
 * 默认时间窗缓存（毫秒）。0 表示禁用。
 * 运行时：[general] gop_cache_sec → 毫秒，经 zcs_gop_queue_set_default_cache_ms。
 */
#define ZCS_GOP_QUEUE_DEFAULT_CACHE_MS 0u

/** 读者槽位初始容量；按需动态扩容。 */
#define ZCS_GOP_QUEUE_READERS_INIT_CAP 64u

/** 直播读者滞后超过此阈值时向直播边缘 snap。 */
#define ZCS_GOP_QUEUE_PLAY_MAX_LAG 240u

/** RTP 重排序 jitter 窗口；槽位数与 GOP ring 相同。 */
#define ZCS_RTP_JITTER_SLOTS_DEFAULT ZCS_GOP_QUEUE_CAPACITY

#endif /* ZCS_CORE_GOP_LIMITS_H */
