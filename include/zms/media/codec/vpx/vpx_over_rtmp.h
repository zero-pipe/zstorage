/**
 * @file vpx_over_rtmp.h
 * @brief VP8/VP9 在 RTMP/FLV 的 config 与序列头。
 */
#ifndef ZMS_MEDIA_CODEC_VPX_VPX_OVER_RTMP_H
#define ZMS_MEDIA_CODEC_VPX_VPX_OVER_RTMP_H

#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从 video config 中定位 VPxC（VP codec configuration record）字节。
 * @param vpxc/vpxc_len 输出借用指针（指向入参 data，不拷贝）
 * @return 1 找到且非空；0 未找到、codec 非 VP8/VP9 或参数非法
 */
ZMS_API int zms_vpx_over_rtmp_config_extradata(zms_codec_id codec, const uint8_t *data, size_t len,
                                               const uint8_t **vpxc, size_t *vpxc_len);

/**
 * 生成 FLV VPx sequence header（video tag 头 + VPxC）。
 * 若 cfg 已是合法的 FLV sequence header 则原样复制；否则从 vpxC 重新封装。
 * @return ZN_OK；参数/codec/数据非法 ZN_ERR_INVALID；cap 不足 ZN_ERR_BUFFER_TOO_SMALL
 */
ZMS_API zn_err_t zms_vpx_flv_sequence_header(zms_codec_id codec, const uint8_t *cfg,
                                              size_t cfg_len, uint8_t *out, size_t cap,
                                              size_t *out_len);

/**
 * 给一帧 VPx ES 加上 FLV video tag 头（key 决定帧类型），写入 out。
 * @return ZN_OK；参数/codec 非法 ZN_ERR_INVALID；cap < len+8 ZN_ERR_BUFFER_TOO_SMALL
 */
ZMS_API zn_err_t zms_vpx_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *video_cfg,
                                            size_t cfg_len, const uint8_t *es, size_t len, int key,
                                            uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_VPX_VPX_OVER_RTMP_H */
