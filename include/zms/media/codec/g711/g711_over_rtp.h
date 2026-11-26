/**
 * @file g711_over_rtp.h
 * @brief G.711 over RTP 的 PT 与解包。
 */
#ifndef ZMS_MEDIA_CODEC_G711_G711_OVER_RTP_H
#define ZMS_MEDIA_CODEC_G711_G711_OVER_RTP_H

#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 返回 G.711 编解码器对应的默认 RTP 负载类型（G711U→PCMU，其它→PCMA）。 */
ZMS_API uint8_t zms_g711_over_rtp_default_pt(zms_codec_id codec);
/** 由 RTP 负载类型反查 G.711 编解码器；非 PCMU/PCMA 返回 ZMS_CODEC_INVALID。 */
ZMS_API zms_codec_id zms_g711_codec_from_rtp_pt(uint8_t pt);
/**
 * 解包 G.711 RTP 负载：负载即裸 PCM，直接返回借用视图。
 * @return ZN_OK 成功；参数非法返回 INVALID。
 */
ZMS_API zn_err_t zms_g711_over_rtp_unpack_payload(const uint8_t *payload, size_t len,
                                                   const uint8_t **g711, size_t *g711_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_G711_G711_OVER_RTP_H */
