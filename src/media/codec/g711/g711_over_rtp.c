/**
 * @file g711_over_rtp.c
 * @brief G.711 over RTP 的 PT 映射与解包。
 */
#include "zcs/media/codec/g711/g711_over_rtp.h"
#include "zcs/media/clock.h"
#include "zm/transport/rtp.h"
#include <stdlib.h>
#include <string.h>

uint8_t zcs_g711_over_rtp_default_pt(zcs_codec_id codec)
{
    if (codec == ZCS_CODEC_G711U) {
        return RTP_PAYLOAD_PCMU;
    }
    return RTP_PAYLOAD_PCMA;
}

zcs_codec_id zcs_g711_codec_from_rtp_pt(uint8_t pt)
{
    if (pt == RTP_PAYLOAD_PCMU) {
        return ZCS_CODEC_G711U;
    }
    if (pt == RTP_PAYLOAD_PCMA) {
        return ZCS_CODEC_G711A;
    }
    return ZCS_CODEC_INVALID;
}

zn_err_t zcs_g711_over_rtp_unpack_payload(const uint8_t *payload, size_t len, const uint8_t **g711,
                                           size_t *g711_len)
{
    if (!payload || len == 0 || !g711 || !g711_len) {
        return ZN_ERR_INVALID;
    }
    *g711 = payload;
    *g711_len = len;
    return ZN_OK;
}
