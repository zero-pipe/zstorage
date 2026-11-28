/**
 * @file codec_id.h
 * @brief codec id 与名称 / track 类型映射。
 */
#ifndef ZCS_MEDIA_CODEC_CODEC_ID_H
#define ZCS_MEDIA_CODEC_CODEC_ID_H

/**
 * 生产路径编解码 ID（H.264 / H.265 + AAC）。
 */
#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_track_type {
    ZCS_TRACK_INVALID = 0,
    ZCS_TRACK_VIDEO = 1,
    ZCS_TRACK_AUDIO = 2,
} zcs_track_type;

typedef enum zcs_codec_id {
    ZCS_CODEC_INVALID = 0,
    ZCS_CODEC_H264 = 1,
    ZCS_CODEC_H265 = 2,
    ZCS_CODEC_AAC = 3,
    ZCS_CODEC_OPUS = 4,
    ZCS_CODEC_G711A = 5,
    ZCS_CODEC_G711U = 6,
    ZCS_CODEC_AV1 = 7,
    ZCS_CODEC_VP8 = 8,
    ZCS_CODEC_VP9 = 9,
    ZCS_CODEC_H266 = 10,
} zcs_codec_id;

typedef struct zcs_codec_meta {
    zcs_codec_id id;
    zcs_track_type track;
    const char *name;
    const char *sdp_mime;
    int mpeg_psi_id;
} zcs_codec_meta;

/** 取编解码元数据（静态表借用指针，勿释放）；未知 id 返回 NULL。 */
ZCS_API const zcs_codec_meta *zcs_codec_meta_get(zcs_codec_id id);
/**
 * 由名称或 SDP mime 反查 codec id（大小写不敏感），并对含 711 的名称做 A/U 启发式判定。
 * @return 匹配的 codec id；未知返回 ZCS_CODEC_INVALID。
 */
ZCS_API zcs_codec_id zcs_codec_id_from_name(const char *name);
/** 取编解码短名（静态借用，勿释放）；未知 id 返回 "invalid"。 */
ZCS_API const char *zcs_codec_name(zcs_codec_id id);
/** 取编解码所属轨道类型；未知 id 返回 ZCS_TRACK_INVALID。 */
ZCS_API zcs_track_type zcs_codec_track_type(zcs_codec_id id);
/** @return @a id 的 MPEG-TS stream_type；未走 TS 则为 0。 */
ZCS_API int zcs_codec_mpeg_psi(zcs_codec_id id);
/** @return MPEG-TS stream_type 对应的 zcs_codec_id（含 _subset/AAC-LATM 变体），或 ZCS_CODEC_INVALID。 */
ZCS_API zcs_codec_id zcs_codec_from_mpeg_psi(int psi);
/** 编解码元数据条目总数，用于按 id 遍历元数据表。 */
ZCS_API unsigned zcs_codec_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_CODEC_ID_H */
