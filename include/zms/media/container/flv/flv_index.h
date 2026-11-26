/**
 * @file flv_index.h
 * @brief VOD FLV 索引。
 */
#ifndef ZMS_MEDIA_CONTAINER_FLV_FLV_INDEX_H
#define ZMS_MEDIA_CONTAINER_FLV_FLV_INDEX_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_flv_index {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
    size_t metadata_bytes;
} zms_vod_flv_index;

/** HTTP 会话内相对索引起点（filepositions 相对本响应 byte 0） */
typedef struct zms_vod_flv_index_view {
    double *times;
    double *filepositions;
    size_t count;
    double filesize;
} zms_vod_flv_index_view;

/** 释放 FLV 索引（times/filepositions 及其本身）；对 NULL 安全。 */
ZMS_API void zms_vod_flv_index_free(zms_vod_flv_index *idx);
/** 按 MP4 sample 表估算 FLV keyframes 索引（times/filepositions 供 HTTP-FLV seek）。
 *  @param mp4_demux MP4 容器 demux ctx（仅 MP4 有效），经 container/mp4 sample 迭代器遍历。 */
ZMS_API zms_vod_flv_index *zms_vod_flv_index_build(void *mp4_demux, size_t video_cfg_len,
                                                   size_t audio_cfg_len, size_t metadata_bytes);
/** 按播放毫秒线性插值估算对应 FLV 字节偏移；索引为空返回 0。 */
ZMS_API double zms_vod_flv_index_byte_at_ms(const zms_vod_flv_index *idx, uint64_t play_ms);
/**
 * 从 play_ms 起播构造相对索引视图：filepositions 相对本响应的 byte 0。
 * @return 视图实例（用 view_free 释放）；索引为空或分配失败返回 NULL。
 */
ZMS_API zms_vod_flv_index_view *zms_vod_flv_index_view_create(const zms_vod_flv_index *idx,
                                                              uint64_t play_ms);
/** 释放相对索引视图；对 NULL 安全。 */
ZMS_API void zms_vod_flv_index_view_free(zms_vod_flv_index_view *view);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CONTAINER_FLV_FLV_INDEX_H */
