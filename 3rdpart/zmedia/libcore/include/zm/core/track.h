#ifndef ZM_CORE_TRACK_H
#define ZM_CORE_TRACK_H

/**
 * @file track.h
 * @brief 轨道描述与归一化的 codec config。
 *
 * reader 在边界把容器原生 config（FLV 包裹 / avcC / hvcC / esds ...）归一化为
 * 这里的 AVCC/HVCC/VVCC/AV1C/ASC/OPUS_HEAD，writer 再按需序列化。
 */
#include "zm/core/types.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zm_config_fmt {
    ZM_CONFIG_NONE = 0,
    ZM_CONFIG_AVCC,
    ZM_CONFIG_HVCC,
    ZM_CONFIG_VVCC,
    ZM_CONFIG_AV1C,
    ZM_CONFIG_VPCC,
    ZM_CONFIG_ASC,
    ZM_CONFIG_OPUS_HEAD,
} zm_config_fmt;

typedef struct zm_config {
    zm_config_fmt fmt;
    uint8_t *data;   /**< 原始记录；由 zm_track 拥有 */
    size_t len;
} zm_config;

typedef struct zm_track {
    uint32_t track_id;
    zm_codec_id codec;
    zm_kind kind;
    int clock_hz;      /**< video 90000；audio = 采样率 */
    int width;
    int height;
    int sample_rate;
    int channels;
    int bits_per_sample;
    zm_timebase timebase;
    zm_config config;
} zm_track;

ZM_API void zm_track_init(zm_track *t);
ZM_API void zm_track_clear(zm_track *t);

/** 复制一份 config 到 track；旧的 config 被释放。 */
ZM_API int zm_track_set_config(zm_track *t, zm_config_fmt fmt, const void *data, size_t len);

/** 深拷贝 config 数据（目标已有数据会被释放）。 */
ZM_API int zm_config_copy(zm_config *dst, const zm_config *src);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_TRACK_H */
