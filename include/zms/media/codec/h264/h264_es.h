/**
 * @file h264_es.h
 * @brief H.264 Annex-B 判断与参数集提取。
 */
#ifndef ZMS_MEDIA_CODEC_H264_H264_ES_H
#define ZMS_MEDIA_CODEC_H264_H264_ES_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从 Annex-B ES 中提取 SPS 与 PPS（借用视图，指向输入）。
 * @return 1 同时找到 SPS 与 PPS；0 缺失。
 */
ZMS_API int zms_h264_annexb_extract_sps_pps(const uint8_t *annexb, size_t len, const uint8_t **sps,
                                            size_t *sps_len, const uint8_t **pps, size_t *pps_len);

/** ES 头部为 Annex-B 起始码（区分 0x00000001 AVCC 长度前缀）。 */
ZMS_API int zms_h264_es_is_annexb(const uint8_t *data, size_t len);

/** Annex-B 或 AVCC 长度前缀 elementary stream。 */
ZMS_API int zms_h264_es_extract_sps_pps(const uint8_t *data, size_t len, const uint8_t **sps,
                                        size_t *sps_len, const uint8_t **pps, size_t *pps_len);

/** 原始 AVCDecoderConfigurationRecord（MPEG-TS PMT esinfo），非 FLV 封装。 */
ZMS_API int zms_h264_avcc_extract_sps_pps(const uint8_t *avcc, size_t len, const uint8_t **sps,
                                          size_t *sps_len, const uint8_t **pps, size_t *pps_len);

/** 检测 ES 是否含 VCL 切片（Annex-B 查 NAL 1/5；否则按 AVCC 解析）。 */
ZMS_API int zms_h264_es_has_slice(const uint8_t *data, size_t len);

/** annex-B ES 中首个 VCL NAL 的 first_mb_in_slice 置位（新访问单元）。 */
ZMS_API int zms_h264_annexb_first_slice(const uint8_t *annexb, size_t len);

/** ES 开始新 H264 访问单元时为真（MPEG-TS PES 边界提示）。 */
ZMS_API int zms_h264_es_starts_access_unit(const uint8_t *es, size_t len);

/**
 * 把任意 ES（Annex-B 或 AVCC）规整为 Annex-B 写入 out。
 * Annex-B 输入为直接拷贝（out 可与 data 相同则原地返回）；AVCC 输入按长度前缀转换。
 * @return ZN_OK 成功；BUFFER_TOO_SMALL 或非法输入失败。
 */
ZMS_API zn_err_t zms_h264_es_to_annexb(const uint8_t *data, size_t len, uint8_t *out, size_t cap,
                                        size_t *out_len);

/** IDR 或 non-IDR I 切片（GOP 起播/sync） */
ZMS_API int zms_h264_annexb_is_sync_key(const uint8_t *annexb, size_t len);

/** 点播起播：access unit 须含 NAL type 5 (IDR) */
ZMS_API int zms_h264_annexb_is_idr(const uint8_t *annexb, size_t len);

/**
 * 在 body（Annex-B）前拼上 SPS/PPS 参数集后输出；若 body 已含 SPS/PPS 则直接拷贝。
 * 用于无带内参数集的 ES 补齐。
 * @return ZN_OK 成功；BUFFER_TOO_SMALL 或 body 为空失败。
 */
ZMS_API zn_err_t zms_h264_annexb_prepend_sps_pps(const uint8_t *sps, size_t sps_len,
                                                  const uint8_t *pps, size_t pps_len,
                                                  const uint8_t *body, size_t body_len,
                                                  uint8_t *out, size_t cap, size_t *out_len);

/** RTSP/RTP：仅保留 VCL NAL（type 1–5），参数集由 SDP sprop 提供 */
ZMS_API zn_err_t zms_h264_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out,
                                           size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_H264_H264_ES_H */
