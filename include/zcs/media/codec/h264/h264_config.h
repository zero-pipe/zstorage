#ifndef ZCS_MEDIA_CODEC_H264_H264_CONFIG_H
#define ZCS_MEDIA_CODEC_H264_H264_CONFIG_H

/**
 * @file h264_config.h
 * @brief H.264 参数集配置接口（基于 zm libflv mpeg4_avc_t）。
 *
 * 使业务/播放层无需包含 `mpeg4-avc.h`。以稳定地址按值持有 `zcs_avc_config` 并传其指针；
 * 加载后切勿拷贝（库结构体内指针指向自有缓冲）。
 */
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按值不透明包装 `struct mpeg4_avc_t`；h264_config.c 断言尺寸匹配。 */
typedef struct zcs_avc_config {
    union {
        uint8_t bytes[10240];
        uint64_t align_;
    } opaque;
} zcs_avc_config;

/** 解析 AVCDecoderConfigurationRecord（avcC）。@return >0 成功，<=0 错误。 */
ZCS_API int zcs_avc_config_load_record(zcs_avc_config *c, const uint8_t *avcc, size_t len);

/** 从 Annex-B 码流解析 SPS/PPS。@return >0 成功，<=0 错误。 */
ZCS_API int zcs_avc_config_load_annexb(zcs_avc_config *c, const uint8_t *annexb, size_t len);

/** @return NAL 长度前缀字节数（1..4），未设置则为 0。 */
ZCS_API int zcs_avc_config_nalu_length(const zcs_avc_config *c);

/** AVCC 长度前缀样本 → Annex-B（按配置前置 SPS/PPS）。@return >0 字节数，<=0 错误。 */
ZCS_API int zcs_avc_config_mp4_to_annexb(const zcs_avc_config *c, const uint8_t *mp4, size_t len,
                                         uint8_t *out, size_t cap);

/** Annex-B AU → AVCC 长度前缀（更新配置 SPS/PPS）。@p vcl/@p update 可选。@return >0 字节数。 */
ZCS_API int zcs_avc_config_annexb_to_mp4(zcs_avc_config *c, const uint8_t *annexb, size_t len,
                                         uint8_t *out, size_t cap, int *vcl, int *update);

/** 仅用 NAL 长度前缀做裸 AVCC→Annex-B（无 SPS/PPS），供格式探测。 */
ZCS_API int zcs_avc_nalu_to_annexb(int nalu_length, const uint8_t *mp4, size_t len, uint8_t *out,
                                   size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H264_H264_CONFIG_H */
