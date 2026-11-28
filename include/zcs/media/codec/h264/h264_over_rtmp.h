/**
 * @file h264_over_rtmp.h
 * @brief H.264 在 RTMP/FLV 的 AVCC 与打包。
 */
#ifndef ZCS_MEDIA_CODEC_H264_H264_OVER_RTMP_H
#define ZCS_MEDIA_CODEC_H264_H264_OVER_RTMP_H

/**
 * H.264 Annex-B over RTMP/FLV media tag body：pack / unpack。
 * 见 Annex-B 工具于 zcs/media/codec/h264/h264_es.h。
 */
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用 SPS/PPS 生成 FLV 视频序列头（AVCDecoderConfigurationRecord），带 FLV tag 头。
 * @return ZN_OK 成功；参数非法/BUFFER_TOO_SMALL 失败。
 */
ZCS_API zn_err_t zcs_rtmp_avc_seq_header(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                          size_t pps_len, uint8_t *out, size_t cap,
                                          size_t *out_len);
/**
 * Annex-B ES 直接打为 FLV 视频 tag（无预载 AVCC，内部自建空 avc）。
 * @param key 非 0 标记关键帧
 */
ZCS_API zn_err_t zcs_rtmp_h264_annexb(const uint8_t *annexb, size_t len, uint32_t tag_dts_ms,
                                       int key, uint8_t *out, size_t cap, size_t *out_len);
/** video_config 预载 AVCC 后打包 VCL（HTTP-FLV/RTMP play，seq header 已单独发送） */
ZCS_API zn_err_t zcs_h264_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                             const uint8_t *annexb, size_t len, int key,
                                             uint8_t *out, size_t cap, size_t *out_len);
/**
 * 将 FLV 视频 tag 体（AVCC）解为 Annex-B ES。
 * @param keyframe 输出是否关键帧，可为 NULL
 * @return ZN_OK 成功；非 AVPACKET/非法/超容量失败。
 */
ZCS_API zn_err_t zcs_rtmp_video_tag_to_annexb(const uint8_t *body, size_t len, uint8_t *out,
                                               size_t cap, size_t *out_len, int *keyframe);
/**
 * 从 AVCC 序列头里提取 SPS/PPS。
 * @return 1 成功（指针借用输入 data）；0 非序列头/无参数集。
 */
ZCS_API int zcs_rtmp_avc_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                         size_t *sps_len, const uint8_t **pps, size_t *pps_len);
/** 从 tag 数据定位 AVCDecoderConfigurationRecord（借用视图）；返回是否找到。 */
ZCS_API int zcs_rtmp_avc_extradata(const uint8_t *data, size_t len, const uint8_t **avcc,
                                   size_t *avcc_len);
/** 解析 profile/compatibility/level 三元组，格式化为 6 位十六进制串；成功返回 1。 */
ZCS_API int zcs_rtmp_avc_profile_level_id(const uint8_t *data, size_t len, char profile[16]);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H264_H264_OVER_RTMP_H */
