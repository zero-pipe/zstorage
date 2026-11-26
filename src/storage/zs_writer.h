/**
 * @file zs_writer.h
 * @brief 单通道写入器：PS pack 聚合写 + 关键帧索引 + 槽位切换。
 *
 * 用法（与 ZMS 的 PS muxer 对接）：
 *
 *   每帧开始： zs_writer_begin_frame(w, keyframe, pts, utc, now);
 *   每包回调： zs_writer_push(w, ps_bytes, len, now);      // 来自 ps muxer 写回调
 *   定时心跳： zs_writer_tick(w, now);
 *   结束时  ： zs_writer_close(w);
 *
 * 关键约定：
 *   - 一个 PS pack 的写回调**不知道**自己是否属于关键帧，所以用 begin_frame
 *     提前打标：begin_frame(keyframe=1) 之后的第一个 pack 才会被记成索引；
 *   - 槽位切换只在关键帧边界发生，保证每个槽位都从 IDR 起播；
 *   - 通道刚起来时如果不是关键帧，这些帧直接丢弃，等到第一个关键帧再开槽。
 */
#ifndef ZMS_SRC_STORAGE_ZS_WRITER_H
#define ZMS_SRC_STORAGE_ZS_WRITER_H

#include "storage/zs_pool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zs_writer zs_writer;

typedef struct zs_writer_opts {
    zs_pool *pool;
    uint32_t channel_id;
    const char *stream;        /**< 流标识，写进槽位头供回放检索（必填） */
    uint32_t flush_bytes;      /**< 聚合写阈值，0 取 512KB */
    uint32_t flush_ms;         /**< 最大刷盘延迟，0 取 1000ms */
    uint32_t slot_max_ms;      /**< 单槽位最长时长，0 取 15 分钟 */
    uint64_t slot_seal_margin; /**< 数据区剩余低于该值提前封口，0 取 4MB */
} zs_writer_opts;

zs_writer *zs_writer_create(const zs_writer_opts *opts);
void zs_writer_destroy(zs_writer *w);

/** 一帧开始；keyframe=1 时下一个 push 的 pack 记为关键帧索引。 */
void zs_writer_begin_frame(zs_writer *w, int keyframe, int64_t pts_90k, int64_t utc_ms,
                           uint64_t now_ms);

/** 推入一个完整 PS pack。@return 0 成功（含被丢弃）；-1 写盘失败 */
int zs_writer_push(zs_writer *w, const void *ps, size_t len, uint64_t now_ms);

/** 定时心跳：到 flush_ms 就刷盘（不切槽位）。 */
int zs_writer_tick(zs_writer *w, uint64_t now_ms);

/** 强制刷盘。 */
int zs_writer_flush(zs_writer *w, uint64_t now_ms);

/** 结束：刷盘 + 归还当前槽位（封口）。 */
void zs_writer_close(zs_writer *w);

/** 当前正在写的槽位 id；未开槽返回 0。 */
uint32_t zs_writer_slot_id(const zs_writer *w);
/** 累计切槽次数，用于观测。 */
uint32_t zs_writer_rotations(const zs_writer *w);
/** 累计写入字节（含尚未落盘的缓冲）。 */
uint64_t zs_writer_bytes(const zs_writer *w);
/** 是否有活跃槽位。 */
int zs_writer_active(const zs_writer *w);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_STORAGE_ZS_WRITER_H */
