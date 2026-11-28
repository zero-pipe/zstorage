/**
 * @file g711_over_rtmp.c
 * @brief G.711 在 RTMP 的打包。
 */
#include "zcs/media/codec/g711/g711_over_rtmp.h"
#include "zm/format/flv.h"
#include <string.h>

static uint8_t g711_flv_audio_hdr(zcs_codec_id codec)
{
    uint8_t fmt = (codec == ZCS_CODEC_G711U) ? FLV_AUDIO_G711U : FLV_AUDIO_G711A;
    return (uint8_t)(fmt | (FLV_SOUND_RATE_5500 << 2) | (FLV_SOUND_BIT_16 << 1) |
                     FLV_SOUND_CHANNEL_MONO);
}

zn_err_t zcs_g711_over_rtmp_pack_es(zcs_codec_id codec, const uint8_t *g711, size_t len,
                                     uint32_t tag_dts_ms, uint8_t *out, size_t cap, size_t *out_len)
{
    (void)tag_dts_ms;
    if (!g711 || len == 0 || !out || (codec != ZCS_CODEC_G711A && codec != ZCS_CODEC_G711U)) {
        return ZN_ERR_INVALID;
    }
    if (cap < 1 + len) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    out[0] = g711_flv_audio_hdr(codec);
    memcpy(out + 1, g711, len);
    if (out_len) {
        *out_len = 1 + len;
    }
    return ZN_OK;
}
