/**
 * @file h265_over_rtmp.h
 * @brief H.265 在 RTMP/FLV 的 HVCC 与打包。
 */
#ifndef ZCS_MEDIA_CODEC_H265_H265_OVER_RTMP_H
#define ZCS_MEDIA_CODEC_H265_H265_OVER_RTMP_H

#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** raw hvcC 或 FLV sequence header → hvcC 指针 */
ZCS_API int zcs_rtmp_hevc_extradata(const uint8_t *data, size_t len, const uint8_t **hvcc,
                                    size_t *hvcc_len);

/** ring/RTSP 存 raw hvcC 时，生成 Enhanced RTMP video tag body（sequence header） */
ZCS_API zn_err_t zcs_h265_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                               size_t cap, size_t *out_len);

/** video_config 预载 hvcC 后打包 VCL（Enhanced RTMP） */
ZCS_API zn_err_t zcs_h265_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H265_H265_OVER_RTMP_H */
