/**
 * @file g711_over_rtmp.h
 * @brief G.711 在 RTMP 的打包。
 */
#ifndef ZMS_MEDIA_CODEC_G711_G711_OVER_RTMP_H
#define ZMS_MEDIA_CODEC_G711_G711_OVER_RTMP_H

/** G.711 PCM over RTMP/FLV audio tag body。 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 将 G.711（A/U）PCM 封装为 FLV 音频 tag body（1 字节音频头 + 原始 PCM）。
 * @param codec 须为 ZMS_CODEC_G711A 或 ZMS_CODEC_G711U
 * @param out_len 可为 NULL；成功时写入总长度
 * @return ZN_OK；codec 非法 ZN_ERR_INVALID；cap 不足 ZN_ERR_BUFFER_TOO_SMALL
 */
ZMS_API zn_err_t zms_g711_over_rtmp_pack_es(zms_codec_id codec, const uint8_t *g711, size_t len,
                                             uint32_t tag_dts_ms, uint8_t *out, size_t cap,
                                             size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_G711_G711_OVER_RTMP_H */
