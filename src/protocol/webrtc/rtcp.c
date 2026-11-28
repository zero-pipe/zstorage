/**
 * @file rtcp.c
 * @brief WebRTC RTCP：SR/PLI/NACK/复合包。
 */
#include "protocol/webrtc/media_internal.h"
#include "zcs/media/wire/rtcp.h"
#include "zcs/media/wire/rtp_packet.h"
#include <string.h>

int zcs_webrtc_rtcp_is_psfb_pli(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    if (data[1] != 206) {
        return 0;
    }
    if ((data[0] & 0x1F) != 1) {
        return 0;
    }
    return 1;
}

int zcs_webrtc_rtcp_is_rtpfb_nack(const uint8_t *data, size_t len)
{
    if (!data || len < 12) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    if (data[1] != 205) {
        return 0;
    }
    if ((data[0] & 0x1F) != 1) {
        return 0;
    }
    return 1;
}

int zcs_webrtc_rtcp_is_rtpfb_twcc(const uint8_t *data, size_t len)
{
    if (!data || len < 20) {
        return 0;
    }
    if ((data[0] & 0xC0) != 0x80) {
        return 0;
    }
    if (data[1] != 205) {
        return 0;
    }
    if ((data[0] & 0x1F) != 15) {
        return 0;
    }
    return 1;
}

int zcs_webrtc_rtcp_parse_twcc(const uint8_t *data, size_t len, zcs_webrtc_twcc_summary *out)
{
    size_t off;
    uint16_t count;
    uint16_t idx = 0;
    uint32_t recv = 0;
    uint32_t lost = 0;
    uint32_t recovered = 0;

    if (!out) {
        return -1;
    }
    if (!zcs_webrtc_rtcp_is_rtpfb_twcc(data, len)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->base_seq = (uint16_t)(((uint16_t)data[12] << 8) | data[13]);
    count = (uint16_t)(((uint16_t)data[14] << 8) | data[15]);
    out->pkt_count = count;
    /* 20 字节固定头后是 2 字节状态块（与 librtp tcc01 一致）：
     * bit15=0 run length，bit14..13 为状态，bit12..0 为长度；
     * bit15=1 status vector，bit14=1 时 7 个 2bit 符号，否则 14 个 1bit 符号。 */
    for (off = 20; off + 2 <= len && idx < count; off += 2) {
        uint16_t chunk = (uint16_t)(((uint16_t)data[off] << 8) | data[off + 1]);
        unsigned i;

        if ((chunk & 0x8000u) == 0) {
            unsigned run = chunk & 0x1FFFu;
            unsigned status = (chunk >> 13) & 0x3u;
            unsigned take;

            if (run == 0) {
                break;
            }
            take = run;
            if (take > (unsigned)(count - idx)) {
                take = (unsigned)(count - idx);
            }
            if (status) {
                recv += take;
            } else {
                lost += take;
            }
            idx = (uint16_t)(idx + take);
            out->parsed += take;
            continue;
        }
        if (chunk & 0x4000u) {
            for (i = 0; i < 7 && idx < count; ++i) {
                unsigned symbol = (chunk >> (2u * (6u - i))) & 0x3u;

                if (symbol) {
                    ++recv;
                } else {
                    ++lost;
                }
                ++idx;
                ++out->parsed;
            }
        } else {
            for (i = 0; i < 14 && idx < count; ++i) {
                if (chunk & (uint16_t)(1u << (13u - i))) {
                    ++recv;
                } else {
                    ++lost;
                }
                ++idx;
                ++out->parsed;
            }
        }
    }
    (void)recovered;
    out->received = recv;
    out->lost = lost;
    out->recovered = recovered;
    if (out->parsed == 0) {
        return -1;
    }
    return 0;
}

int zcs_webrtc_rtcp_parse_rr(const uint8_t *data, size_t len, uint32_t *fraction_lost,
                             uint32_t *jitter, uint32_t *lsr, uint32_t *dlsr)
{
    if (!data || len < 32 || (data[0] & 0xC0) != 0x80 || data[1] != 201) {
        return -1;
    }
    if (fraction_lost) {
        *fraction_lost = data[12];
    }
    if (jitter) {
        *jitter = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                  ((uint32_t)data[22] << 8) | data[23];
    }
    if (lsr) {
        *lsr = ((uint32_t)data[24] << 24) | ((uint32_t)data[25] << 16) |
               ((uint32_t)data[26] << 8) | data[27];
    }
    if (dlsr) {
        *dlsr = ((uint32_t)data[28] << 24) | ((uint32_t)data[29] << 16) |
                ((uint32_t)data[30] << 8) | data[31];
    }
    return 0;
}

size_t zcs_webrtc_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                uint32_t octet_count)
{
    return zcs_rtcp_build_sr(out, cap, ssrc, ntp_sec, ntp_frac, rtp_ts, packet_count, octet_count);
}

void zcs_webrtc_rtcp_for_each_compound(const uint8_t *data, size_t len,
                                       void (*cb)(const uint8_t *block, size_t block_len,
                                                  void *user),
                                       void *user)
{
    size_t off = 0;

    if (!data || !cb) {
        return;
    }
    while (off + 4 <= len) {
        uint16_t blk_len = (uint16_t)(((uint16_t)data[off + 2] << 8) | data[off + 3]);
        size_t total = ((size_t)blk_len + 1u) * 4u;
        if (total < 4 || off + total > len) {
            break;
        }
        cb(data + off, total, user);
        off += total;
    }
}
