/**
 * @file aac_over_rtmp.h
 * @brief AAC 在 RTMP/FLV 的 extradata 与打包。
 */
#ifndef ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTMP_H
#define ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTMP_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从 FLV 音频 tag 中定位 AAC AudioSpecificConfig（借用视图）。
 * @return 1 成功（*asc 指向 data 内部）；0 非 AAC/非序列头。
 */
ZCS_API int zcs_rtmp_aac_extradata(const uint8_t *data, size_t len, const uint8_t **asc,
                                   size_t *asc_len);
/** 用 ASC 生成 FLV AAC 序列头（0xAF 0x00 + ASC）。 */
ZCS_API zn_err_t zcs_rtmp_aac_seq_header(const uint8_t *asc, size_t asc_len, uint8_t *out,
                                          size_t cap, size_t *out_len);
/** 将 AAC raw 帧打为 FLV AAC 音频 tag（0xAF 0x01 + payload）。 */
ZCS_API zn_err_t zcs_rtmp_aac_frame(const uint8_t *aac, size_t len, uint32_t tag_dts_ms,
                                     uint8_t *out, size_t cap, size_t *out_len);

/**
 * 解析 AAC AudioSpecificConfig 的采样率与声道数；非法返回 0。
 * @param sample_rate 输出采样率，可为 NULL
 * @param channels    输出声道数，可为 NULL
 */
ZCS_API int zcs_aac_parse_asc(const uint8_t *asc, size_t len, int *sample_rate, int *channels);
/** 解析 ADTS 固定头（7+ 字节）；非 ADTS 或无效时返回 0。 */
ZCS_API int zcs_aac_adts_parse(const uint8_t *adts, size_t len, int *sample_rate, int *channels);
/**
 * 由采样率/声道构造 2 字节 ASC 的十六进制串（如 "1210"）。
 * 采样率/声道非法时回退 44100/2；hex_cap 至少 5。
 * @return 1 成功；缓冲不足返回 0。
 */
ZCS_API int zcs_aac_build_config_hex(int sample_rate, int channels, char *hex, size_t hex_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTMP_H */
