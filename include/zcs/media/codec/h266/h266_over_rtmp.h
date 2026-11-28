/**
 * @file h266_over_rtmp.h
 * @brief H.266 在 RTMP/FLV 的 VVCC 与序列头。
 */
#ifndef ZCS_MEDIA_CODEC_H266_H266_OVER_RTMP_H
#define ZCS_MEDIA_CODEC_H266_H266_OVER_RTMP_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从解码器 extradata 中定位 VVC 配置（VVCC/NAL）位置。
 * @param vvcc/vvcc_len 输出，指向输入 data 内部的借用指针
 * @return 1 找到；0 未找到或参数非法
 */
ZCS_API int zcs_h266_over_rtmp_config_extradata(const uint8_t *data, size_t len,
                                                const uint8_t **vvcc, size_t *vvcc_len);

/** 从视频配置中取 VVCC；等价于 config_extradata，供 FLV 路径复用。@return 1 找到，0 无。 */
ZCS_API int zcs_h266_video_config_vvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **vvcc,
                                       size_t *vvcc_len);

/**
 * 生成 FLV 视频"序列头"tag body（H.266 的 AVCDecoderConfigurationRecord 对应物）。
 * cfg 已是序列头时直接拷贝；否则从 VVCC 重建并加 FLV 视频头。
 * @param out/out_len 输出缓冲与长度；cap 不足返回 ZN_ERR_BUFFER_TOO_SMALL
 * @return ZN_OK；配置非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_h266_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                               size_t cap, size_t *out_len);

/**
 * 将 Annex-B ES 封装为 FLV 视频 tag body；关键帧前附序列头。
 * @param video_cfg 提供 VVCC 的视频配置
 * @param key       非零表示关键帧（会插入配置/序列头）
 * @return ZN_OK；cap 不足 ZN_ERR_BUFFER_TOO_SMALL；配置/数据非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_h266_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H266_H266_OVER_RTMP_H */
