#ifndef ZCS_CORE_FRAME_H
#define ZCS_CORE_FRAME_H

/**
 * @file frame.h
 * @brief 与协议无关的媒体载荷（ES 单元）
 *
 * @ref zcs_frame 携带归一化后的 codec 字节、入站时间戳与 GOP 元数据
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/codec/codec_id.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_gop_slot zcs_gop_slot;

/** ingress 写入并在 gop_queue 中流转的内存 ES 单元 */
typedef struct zcs_frame {
    uint8_t *data;
    size_t size;
    size_t capacity;
    uint64_t dts_ms;
    uint64_t pts_ms;
    zcs_codec_id codec;
    zcs_track_type track;
    int keyframe;
    /** SPS/PPS/VPS/AAC AudioSpecificConfig 等参数集。 */
    int config_frame;
    /** SEI/AUD 等可丢弃单元；不推进 GOP 状态。 */
    int drop_able;
    int owned;
} zcs_frame;

/** 将 frame 全部字段清零（不释放 data）；对 NULL 安全。 */
ZCS_API void zcs_frame_init(zcs_frame *f);
/** 释放自有缓冲（owned 时 free data），再重置为全零；借用（owned=0）时不 free。对 NULL 安全。 */
ZCS_API void zcs_frame_clear(zcs_frame *f);
/**
 * 确保自有缓冲容量 >= cap（realloc；扩容后会置 owned=1）。
 * 已足够则直接返回；对借用数据调用会转为自有分配。cap<=capacity 时无操作。
 * @return ZN_ERR_INVALID f 为空；ZN_ERR_NOMEM 分配失败
 */
ZCS_API zn_err_t zcs_frame_reserve(zcs_frame *f, size_t cap);
/**
 * 把 data 设为 frame 载荷。
 * @param copy 非 0：扩容后拷贝进自有缓冲（owned=1）；0：借用 data（先清旧内容，owned=0），
 *             调用方须保证 data 生命周期覆盖 frame
 * @return ZN_ERR_INVALID f 为空或 data 为空但 len>0；copy 模式下透传 reserve 错误
 */
ZCS_API zn_err_t zcs_frame_assign(zcs_frame *f, const void *data, size_t len, int copy);

/** es_codec 注册表从 ES 内容推导 keyframe 标志（单一 hub，避免各协议重复判定） */
ZCS_API void zcs_frame_refresh_key_from_es(zcs_frame *f);

/** @return 非零表示视频 config keyframe（GOP 边界） */
ZCS_API int zcs_frame_video_gop_marker(const zcs_frame *f);

/** @return 非零表示 caching 已开始后该帧可写入 gop_queue */
ZCS_API int zcs_gop_queue_storable(const zcs_frame *f, int cache_started, int has_video);

/** @return 非零表示该帧在 ring 中开启新 GOP */
ZCS_API int zcs_gop_queue_new_gop(const zcs_frame *f, int video_key_pos, int has_video);

/** @return 非零表示 es_codec 注册表判定为 egress sync 点 */
ZCS_API int zcs_frame_is_egress_sync(const zcs_frame *f);

/**
 * @return 非零表示解码器可仅凭此帧起播（如 H.264/H.265 IDR）
 *
 * 参数集可通过 insertConfig 单独发送；起播判定不要求同包携带
 */
ZCS_API int zcs_frame_is_decode_start(const zcs_frame *f);

/** @return 与 @ref zcs_frame_is_decode_start 相同（首个可解码 VCL） */
ZCS_API int zcs_frame_is_play_start(const zcs_frame *f);

/**
 * 将入站 @a dts_ms / @a pts_ms 归一化为 ring slot 时间轴字段
 * @a pts_ms 与 DTS 相同时为 0（见 @ref zcs_gop_slot.dts_ms）
 */
ZCS_API void zcs_gop_queue_timeline_from_frame(const zcs_frame *f, uint32_t *dts_ms,
                                               uint32_t *pts_ms);

/** @return ring slot DTS（ms），用于交织 / pacing */
ZCS_API uint32_t zcs_gop_slot_dts_ms(const zcs_gop_slot *slot);

/** @return ring slot PTS（ms），用于 mux tag；无 PTS 时回退 DTS */
ZCS_API uint32_t zcs_gop_slot_pts_ms(const zcs_gop_slot *slot);

/** @return 非零表示 slot 为 egress sync 点（egress 重封装起播） */
ZCS_API int zcs_gop_slot_is_egress_sync(const zcs_gop_slot *slot);

/** 若该 slot 为 egress sync 点，则刷新其 keyframe 标志 */
ZCS_API void zcs_gop_slot_refresh_sync_key(zcs_gop_slot *slot);

/** @return 非零表示 slot 为解码起播点（H.264/H.265 IDR 等） */
ZCS_API int zcs_gop_slot_is_decode_start(const zcs_gop_slot *slot);

/** 若该 slot 为解码起播点，则刷新 keyframe 标志 */
ZCS_API void zcs_gop_slot_refresh_decode_key(zcs_gop_slot *slot);

/** @return 非零表示 slot 为播放起播点（首个可解码 VCL） */
ZCS_API int zcs_gop_slot_is_play_start(const zcs_gop_slot *slot);

/** 若该 slot 为播放起播点，则刷新 keyframe 标志 */
ZCS_API void zcs_gop_slot_refresh_play_key(zcs_gop_slot *slot);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_FRAME_H */
