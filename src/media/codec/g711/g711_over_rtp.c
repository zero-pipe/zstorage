/**
 * @file g711_over_rtp.c
 * @brief G.711 over RTP 的 PT 映射与解包。
 */
#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/media/clock.h"
#include "zm/transport/rtp.h"
#include <stdlib.h>
#include <string.h>

uint8_t zms_g711_over_rtp_default_pt(zms_codec_id codec)
{
    if (codec == ZMS_CODEC_G711U) {
        return RTP_PAYLOAD_PCMU;
    }
    return RTP_PAYLOAD_PCMA;
}

zms_codec_id zms_g711_codec_from_rtp_pt(uint8_t pt)
{
    if (pt == RTP_PAYLOAD_PCMU) {
        return ZMS_CODEC_G711U;
    }
    if (pt == RTP_PAYLOAD_PCMA) {
        return ZMS_CODEC_G711A;
    }
    return ZMS_CODEC_INVALID;
}

zn_err_t zms_g711_over_rtp_unpack_payload(const uint8_t *payload, size_t len, const uint8_t **g711,
                                           size_t *g711_len)
{
    if (!payload || len == 0 || !g711 || !g711_len) {
        return ZN_ERR_INVALID;
    }
    *g711 = payload;
    *g711_len = len;
    return ZN_OK;
}
