#ifndef ZCS_MEDIA_CODEC_PARAM_SETS_H
#define ZCS_MEDIA_CODEC_PARAM_SETS_H

/**
 * @file param_sets.h
 * @brief 编解码参数集缓存（H.264 SPS/PPS，H.265 VPS/SPS/PPS）。
 *
 * HLS/DASH/MPEG-TS 出站都需要在关键帧前补齐参数集；这里只做原始 NAL 的
 * 缓存与从 FLV/RTMP video sequence header 的提取，不依赖任何上层模块。
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_codec_param_sets {
    uint8_t *vps;
    uint8_t *sps;
    uint8_t *pps;
    size_t vps_len;
    size_t sps_len;
    size_t pps_len;
} zcs_codec_param_sets;

void zcs_codec_param_sets_init(zcs_codec_param_sets *ps);
void zcs_codec_param_sets_clear(zcs_codec_param_sets *ps);

/** 缓存 FLV/RTMP video sequence header 中的原始参数集（H.264/H.265）。 */
int zcs_codec_param_sets_cache_flv_cfg(zcs_codec_param_sets *ps, const uint8_t *cfg,
                                       size_t clen);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_PARAM_SETS_H */
