#ifndef ZCS_MEDIA_CODEC_AAC_AAC_CONFIG_H
#define ZCS_MEDIA_CODEC_AAC_AAC_CONFIG_H

/**
 * @file aac_config.h
 * @brief AAC 配置接口（基于 zm libflv mpeg4_aac_t 解析器）。
 *
 * 使业务/播放/容器层无需包含 `mpeg4-aac.h`：调用方按值持有 `zcs_aac_config`，
 * 播种默认值 / 加载 AudioSpecificConfig，再序列化 ASC 或 ADTS 头而无需触碰原始库结构体。
 */
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 按值不透明包装 `struct mpeg4_aac_t`。字节存储留足余量；
 * aac_config.c 静态断言可容纳底层库结构体。
 */
typedef struct zcs_aac_config {
    union {
        uint8_t bytes[160];
        uint64_t align_;
    } opaque;
} zcs_aac_config;

/** 重置为 @p sample_rate / @p channels 的 AAC-LC 默认（0 → 44100 / 2）。 */
ZCS_API void zcs_aac_config_set_defaults(zcs_aac_config *c, int sample_rate, int channels);

/** 加载 AudioSpecificConfig。@return 1 成功，0 失败。 */
ZCS_API int zcs_aac_config_load_asc(zcs_aac_config *c, const uint8_t *asc, size_t len);

/** 将 AudioSpecificConfig 序列化到 @p out。@return 写入字节数，或 0。 */
ZCS_API size_t zcs_aac_config_save_asc(const zcs_aac_config *c, uint8_t *out, size_t cap);

/** 为 @p payload_len 字节载荷写 ADTS 头。@return 头字节数（>=7），或 <0。 */
ZCS_API int zcs_aac_config_adts_header(const zcs_aac_config *c, uint16_t payload_len, uint8_t *out,
                                       size_t cap);

/** @return 解码采样率（Hz），未知则 <=0。 */
ZCS_API int zcs_aac_config_sample_rate(const zcs_aac_config *c);

/** @return 声道数（channel_configuration），未设置则为 0。 */
ZCS_API int zcs_aac_config_channels(const zcs_aac_config *c);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_AAC_AAC_CONFIG_H */
