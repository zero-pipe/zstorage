/**
 * @file h265_es.h
 * @brief H.265 Annex-B 判断与参数集提取。
 */
#ifndef ZMS_MEDIA_CODEC_H265_H265_ES_H
#define ZMS_MEDIA_CODEC_H265_H265_ES_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLA / IDR / CRA 等随机接入点（GOP 起播/sync，NAL type 16–21） */
ZMS_API int zms_h265_annexb_is_sync_key(const uint8_t *annexb, size_t len);

/** 点播起播：access unit 须含 IDR 类 NAL（type 19–20） */
ZMS_API int zms_h265_annexb_is_idr(const uint8_t *annexb, size_t len);

/**
 * 从 Annex-B 流中提取 VPS/SPS/PPS。
 * 输出指针是借用（指向入参 annexb 内部，不拷贝）；VPS 可选，SPS/PPS 缺失时对应输出置空。
 * @return 1 表示同时找到 SPS 与 PPS；0 表示未找到或参数不足
 */
ZMS_API int zms_h265_annexb_extract_vps_sps_pps(const uint8_t *annexb, size_t len,
                                                const uint8_t **vps, size_t *vps_len,
                                                const uint8_t **sps, size_t *sps_len,
                                                const uint8_t **pps, size_t *pps_len);

/** RTSP/RTP：仅保留 VCL NAL（type 0–31），去掉 AUD/SEI/参数集等 */
ZMS_API zn_err_t zms_h265_annexb_copy_vcl(const uint8_t *annexb, size_t len, uint8_t *out,
                                           size_t cap, size_t *out_len);

/** sync 帧前 prepend VPS/SPS/PPS，再拼接 VCL-only AU（RFC7798 单 AU 多 RTP） */
ZMS_API zn_err_t zms_h265_annexb_build_rtp_au(const uint8_t *vps, size_t vps_len,
                                               const uint8_t *sps, size_t sps_len,
                                               const uint8_t *pps, size_t pps_len,
                                               const uint8_t *annexb, size_t len,
                                               int prepend_params, uint8_t *out, size_t cap,
                                               size_t *out_len);

/** hvcC extradata → VPS/SPS/PPS 裸 NAL */
ZMS_API int zms_h265_hvcc_param_sets(const uint8_t *hvcc, size_t hvcc_len, const uint8_t **vps,
                                     size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                                     const uint8_t **pps, size_t *pps_len);

/** video_config：raw hvcC 或 legacy FLV sequence header → VPS/SPS/PPS */
ZMS_API int zms_h265_video_config_param_sets(const uint8_t *cfg, size_t cfg_len,
                                             const uint8_t **vps, size_t *vps_len,
                                             const uint8_t **sps, size_t *sps_len,
                                             const uint8_t **pps, size_t *pps_len);

/** video_config → raw hvcC 指针（raw 或从 FLV header 剥离） */
ZMS_API int zms_h265_video_config_hvcc(const uint8_t *cfg, size_t cfg_len, const uint8_t **hvcc,
                                       size_t *hvcc_len);

/**
 * 由 Annex-B 参数集/AU 生成 hvcC（HEVCDecoderConfigurationRecord）。
 * @param out/out_len 输出缓冲与实际长度
 * @return ZN_OK；缺少参数集或 cap 不足导致编码失败返回 ZN_ERR_INVALID
 */
ZMS_API zn_err_t zms_h265_hvcc_from_annexb(const uint8_t *annexb, size_t len, uint8_t *out,
                                            size_t cap, size_t *out_len);
/**
 * 由 VPS/SPS/PPS 生成 hvcC：内部先拼成 Annex-B（限 4KB）再编码。
 * @return ZN_OK；参数缺失 ZN_ERR_INVALID；内部 Annex-B 或输出缓冲不足 ZN_ERR_BUFFER_TOO_SMALL
 */
ZMS_API zn_err_t zms_h265_hvcc_from_param_sets(const uint8_t *vps, size_t vps_len,
                                                const uint8_t *sps, size_t sps_len,
                                                const uint8_t *pps, size_t pps_len, uint8_t *out,
                                                size_t cap, size_t *out_len);

/**
 * 把 VPS/SPS/PPS 拼成 Annex-B（每个 NAL 带起始码）。VPS 可选，SPS/PPS 必需。
 * @return ZN_OK；参数缺失 ZN_ERR_INVALID；cap 不足 ZN_ERR_BUFFER_TOO_SMALL
 */
ZMS_API zn_err_t zms_h265_param_sets_to_annexb(const uint8_t *vps, size_t vps_len,
                                                const uint8_t *sps, size_t sps_len,
                                                const uint8_t *pps, size_t pps_len, uint8_t *out,
                                                size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_H265_H265_ES_H */
