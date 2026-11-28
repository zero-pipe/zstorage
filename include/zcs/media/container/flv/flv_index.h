/**
 * @file flv_index.h
 * @brief VOD FLV 索引。
 */
#ifndef ZCS_MEDIA_CONTAINER_FLV_FLV_INDEX_H
#define ZCS_MEDIA_CONTAINER_FLV_FLV_INDEX_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_flv_index {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
    size_t metadata_bytes;
} zcs_vod_flv_index;

/** HTTP 会话内相对索引起点（filepositions 相对本响应 byte 0） */
typedef struct zcs_vod_flv_index_view {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
} zcs_vod_flv_index_view;

/** 释放 FLV 索引（times/filepositions 及其本身）；对 NULL 安全。 */
ZCS_API void zcs_vod_flv_index_free(zcs_vod_flv_index *idx);
/** 按 MP4 sample 表估算 FLV keyframes 索引（times/filepositions 供 HTTP-FLV seek）。
 *  @param mp4_demux MP4 容器 demux ctx（仅 MP4 有效），经 container/mp4 sample 迭代器遍历。 */
ZCS_API zcs_vod_flv_index *zcs_vod_flv_index_build(void *mp4_demux, size_t video_cfg_len,
                                                   size_t audio_cfg_len, size_t metadata_bytes);
/** 按播放毫秒线性插值估算对应 FLV 字节偏移；索引为空返回 0。 */
ZCS_API double zcs_vod_flv_index_byte_at_ms(const zcs_vod_flv_index *idx, uint64_t play_ms);
/**
 * 从 play_ms 起播构造相对索引视图：filepositions 相对本响应的 byte 0。
 * @return 视图实例（用 view_free 释放）；索引为空或分配失败返回 NULL。
 */
ZCS_API zcs_vod_flv_index_view *zcs_vod_flv_index_view_create(const zcs_vod_flv_index *idx,
                                                              uint64_t play_ms);
/** 释放相对索引视图；对 NULL 安全。 */
ZCS_API void zcs_vod_flv_index_view_free(zcs_vod_flv_index_view *view);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_FLV_FLV_INDEX_H */
