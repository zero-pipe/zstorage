/**
 * @file flv_metadata.h
 * @brief VOD FLV onMetaData 编码。
 */
#ifndef ZCS_VOD_IO_FLV_METADATA_H
#define ZCS_VOD_IO_FLV_METADATA_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_media_source;
struct zcs_vod_flv_index;
struct zcs_vod_flv_index_view;

/** onMetaData script body 大小（含 keyframes） */
ZCS_API size_t zcs_vod_flv_metadata_body_size(const struct zcs_media_source *src,
                                              double duration_sec,
                                              const struct zcs_vod_flv_index *idx);

/** 编码 onMetaData AMF body；返回 0 表示失败 */
ZCS_API size_t zcs_vod_flv_metadata_encode(const struct zcs_media_source *src, uint8_t *out,
                                           size_t cap, double duration_override_sec,
                                           const struct zcs_vod_flv_index *idx,
                                           const struct zcs_vod_flv_index_view *view);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_FLV_METADATA_H */
