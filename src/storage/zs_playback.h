/**
 * @file zs_playback.h
 * @brief 槽位回放：把 .zslot 包装成可被 VOD 链路消费的只读文件视图。
 *
 * VOD 的 file_reader 按扩展名选容器 demuxer，并要求一个"能 open_file 的磁盘路径"。
 * 槽位不是普通容器文件（数据区从固定偏移开始、末尾是预分配的空洞），所以这里
 * 提供两件东西：
 *
 *   1. 槽位元数据查询（按流名/时间找槽位、取该槽位的绝对时间基准）；
 *   2. **按需导出视图**：把某槽位的数据区原样写成一个临时 .ps 文件（或直接
 *      登记为视图路径），供 file_reader 用 PS demuxer 打开。
 *
 * 为什么不直接给 file_reader 一个偏移量：那需要改 file_reader 的 I/O 抽象，
 * 代价远大于"导出一次只读视图"。导出是顺序拷贝 + 后续只读，成本可接受；
 * 需要零拷贝时再演进为自定义 io 适配器（接口位置已留好）。
 */
#ifndef ZCS_SRC_STORAGE_ZS_PLAYBACK_H
#define ZCS_SRC_STORAGE_ZS_PLAYBACK_H

#include "storage/zs_slot.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一段可回放的录像：一个槽位里 [from_ms, to_ms] 的可见区间。 */
typedef struct zs_play_segment {
    uint32_t slot_id;
    char stream[ZS_SLOT_STREAM_MAX];
    /** 槽位内首个关键帧的绝对时间（回放时间基准）。 */
    uint64_t base_utc_ms;
    /** 该槽位实际覆盖的时间范围。 */
    uint64_t start_ms;
    uint64_t end_ms;
    /** 关键帧个数（用于时长估算与观测）。 */
    uint32_t keyframes;
} zs_play_segment;

/**
 * 把槽位数据区导出成标准 PS 文件（供 VOD/ffmpeg 直接打开）。
 * @param max_bytes 导出上限，0 表示整个数据区
 * @return 写出的字节数；失败返回 -1
 */
int64_t zs_playback_export_ps(const zs_slot *s, const char *out_path, uint64_t max_bytes);

/**
 * 按时间裁剪导出：从 <= from_ms 的最近关键帧开始，直到 to_ms（含）之后的下一个
 * 关键帧之前。用于"只播这一段"。
 * @param actual_start 输出：实际起始绝对时间（关键帧时间）
 * @return 写出的字节数；失败返回 -1
 */
int64_t zs_playback_export_range(const zs_slot *s, uint64_t from_ms, uint64_t to_ms,
                                 const char *out_path, uint64_t *actual_start);

/**
 * 把槽位绝对时间换算成该槽位的相对时间（毫秒，从首个关键帧起算）。
 * @return 0 成功；-1 槽位无关键帧
 */
int zs_playback_relative_ms(const zs_slot *s, uint64_t abs_ms, uint64_t *rel_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_STORAGE_ZS_PLAYBACK_H */
