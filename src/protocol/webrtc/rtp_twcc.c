/**
 * @file rtp_twcc.c
 * @brief WebRTC RTP TWCC 扩展。
 */
#include "protocol/webrtc/media_internal.h"
#include <string.h>

#define WEBRTC_TWCC_EXT_ID_DEFAULT 3

size_t zcs_webrtc_rtp_add_twcc_ext(uint8_t *rtp, size_t len, size_t cap, int ext_id,
                                   uint16_t twcc_seq, size_t *out_len)
{
    size_t hdr_len;
    size_t ext_words;
    size_t new_len;
    uint8_t tmp[2048];

    if (!rtp || len < 12 || len > cap || !out_len) {
        return 0;
    }
    if (ext_id <= 0 || ext_id > 14) {
        ext_id = WEBRTC_TWCC_EXT_ID_DEFAULT;
    }
    if ((rtp[0] & 0x10) != 0) {
        return len;
    }
    hdr_len = 12;
    new_len = hdr_len + 4 + 4;
    if (new_len > cap || new_len > sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, rtp, len);
    memmove(tmp + new_len, tmp + hdr_len, len - hdr_len);
    tmp[0] = (uint8_t)(tmp[0] | 0x10);
    tmp[hdr_len + 0] = 0xBE;
    tmp[hdr_len + 1] = 0xDE;
    ext_words = 1;
    tmp[hdr_len + 2] = (uint8_t)(ext_words >> 8);
    tmp[hdr_len + 3] = (uint8_t)(ext_words & 0xff);
    tmp[hdr_len + 4] = (uint8_t)(((unsigned)ext_id << 4) | 1);
    tmp[hdr_len + 5] = (uint8_t)(twcc_seq >> 8);
    tmp[hdr_len + 6] = (uint8_t)(twcc_seq & 0xff);
    tmp[hdr_len + 7] = 0;
    new_len = hdr_len + 4 + 4 + (len - hdr_len);
    memcpy(rtp, tmp, new_len);
    *out_len = new_len;
    return new_len;
}
