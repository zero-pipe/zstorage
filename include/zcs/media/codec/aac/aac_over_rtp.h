/**
 * @file aac_over_rtp.h
 * @brief AAC ES 遍历与 raw 规范化（RTP 用）。
 */
#ifndef ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTP_H
#define ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTP_H

/**
 * AAC over RTP：
 * - 无状态：ES 遍历 / raw 规范化
 * - 有状态 demuxer：RTP 包 → AAC AU（librtp mpeg4-generic，内置 jitter/reorder）
 * PLAY 打包由 rtp_payload_muxer（zm_rtp_writer）处理。
 */
#include "zcs/zcs_export.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*zcs_aac_au_cb)(const uint8_t *au, size_t len, void *user);

/**
 * 遍历 AAC ES，逐帧回调 raw AU（去 ADTS 头）。
 * 若 ES 以 ADTS 同步开头则按帧拆分逐个回调；否则整段作为一个 AU 回调一次。
 * @param cb 返回非 0 时提前结束
 * @return ZN_OK；参数非法或回调失败返回 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_aac_es_foreach_frame(const uint8_t *es, size_t len, zcs_aac_au_cb cb,
                                           void *user);

/** CC gap：要求 PES payload 被 ADTS 帧完整填满，拒绝半帧/脏数据。 */
ZCS_API int zcs_aac_es_strict_valid(const uint8_t *es, size_t len);

/** ring ES 规范化：若含 ADTS 则返回 raw AAC 区（无 ADTS）；否则原样。 */
ZCS_API zn_err_t zcs_aac_es_to_raw(const uint8_t *es, size_t len, const uint8_t **raw,
                                    size_t *raw_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_AAC_AAC_OVER_RTP_H */
