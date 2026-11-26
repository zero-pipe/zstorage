/**
 * @file zs_clock.h
 * @brief 7x24 时间戳归一化：相机 90kHz PTS -> 单调 64 位 PTS + UTC 毫秒。
 *
 * 相机/上级平台给的原始 PTS 不能直接当存储索引：可能从 0 起、带随机基准、
 * 33 位回绕（26.5 小时一圈）、重启归零、甚至比服务器墙钟快慢几分钟。
 * 存储层只认一套时间：
 *
 *   - pts_90k：64 位单调递增，永不回绕；
 *   - utc_ms ：与 pts_90k 一一对应的墙钟毫秒，用于按时间检索。
 *
 * 本模块把原始 PTS 拆成两部分用途：
 *   - 帧间隔用 raw PTS 的增量（展开 33 位回绕得到）；
 *   - 绝对时间锚在服务器墙钟，断流/归零/倒退时重锚并置 discontinuity。
 *
 * 目标是"不倒退、不漂、能跨 26.5 小时"，不追求与墙钟的极限精度。
 */
#ifndef ZMS_SRC_STORAGE_ZS_CLOCK_H
#define ZMS_SRC_STORAGE_ZS_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zs_clock zs_clock;

typedef struct zs_clock_cfg {
    /** 相邻帧 PTS 增量超过该毫秒数即判定断流（默认 2000）。 */
    uint32_t gap_ms;
    /** 归一化时间落后墙钟超过该毫秒数即重锚（默认 3000）。 */
    uint32_t resync_ms;
} zs_clock_cfg;

typedef struct zs_clock_out {
    /** 64 位单调 90kHz 时间戳（永不回绕）。 */
    int64_t pts_90k;
    /** 与 pts_90k 对应的 UTC 毫秒。 */
    int64_t utc_ms;
    /** 本次输入发生了重锚（断流/归零/追墙钟）。 */
    int discontinuity;
    /** 输出有效。 */
    int valid;
} zs_clock_out;

/** @param cfg 传 NULL 用默认值（gap_ms=2000, resync_ms=3000）。 */
zs_clock *zs_clock_create(const zs_clock_cfg *cfg);
void zs_clock_destroy(zs_clock *c);

/** 清空锚点，下一次 input 重新以墙钟为基准。 */
void zs_clock_reset(zs_clock *c);

/**
 * 输入一帧时间戳。
 * @param raw_pts_90k  相机原始 90kHz PTS（允许 33 位或 64 位，内部会取低 33 位算增量）
 * @param wall_ms      当前服务器墙钟毫秒
 * @param frame_dur_ms 该帧的标称时长，用于重锚时推进一步；0 表示按 25fps(40ms)
 * @param out          输出；允许 NULL（只推进内部状态）
 */
void zs_clock_input(zs_clock *c, int64_t raw_pts_90k, int64_t wall_ms, uint32_t frame_dur_ms,
                    zs_clock_out *out);

/** 累计重锚次数，用于运行期告警。 */
uint32_t zs_clock_discontinuity_count(const zs_clock *c);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_STORAGE_ZS_CLOCK_H */
