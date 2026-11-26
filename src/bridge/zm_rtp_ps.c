/**
 * @file zm_rtp_ps.c
 * @brief GB28181 RTP/PS 打包：PS pack → RTP 分片。
 */
#include "bridge/zm_rtp_ps.h"

#include "rtp-header.h"
#include "rtp-packet.h"

#include <stdlib.h>
#include <string.h>

#define ZMS_RTP_PS_MTU_DEFAULT 1400u

struct zms_zm_rtp_ps {
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    size_t mtu;
    zms_zm_rtp_ps_write_fn write;
    void *user;
    uint8_t *packet;
};

zms_zm_rtp_ps *zms_zm_rtp_ps_create(const zms_zm_rtp_ps_opts *opts)
{
    zms_zm_rtp_ps *s;

    if (!opts || !opts->write) {
        return NULL;
    }
    s = (zms_zm_rtp_ps *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->payload_type = opts->payload_type;
    s->ssrc = opts->ssrc;
    s->seq = opts->seq;
    s->mtu = opts->mtu ? opts->mtu : ZMS_RTP_PS_MTU_DEFAULT;
    if (s->mtu <= RTP_FIXED_HEADER + 16) {
        s->mtu = ZMS_RTP_PS_MTU_DEFAULT;
    }
    s->write = opts->write;
    s->user = opts->user;
    s->packet = (uint8_t *)malloc(s->mtu);
    if (!s->packet) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_zm_rtp_ps_destroy(zms_zm_rtp_ps *s)
{
    if (!s) {
        return;
    }
    free(s->packet);
    free(s);
}

uint16_t zms_zm_rtp_ps_last_seq(const zms_zm_rtp_ps *s)
{
    return s ? s->seq : 0;
}

int zms_zm_rtp_ps_input(zms_zm_rtp_ps *s, const uint8_t *ps, size_t len, uint32_t timestamp)
{
    size_t off = 0;
    size_t chunk_max;

    if (!s || !ps || len == 0) {
        return -1;
    }
    chunk_max = s->mtu - RTP_FIXED_HEADER;

    while (off < len) {
        struct rtp_packet_t pkt;
        size_t chunk = len - off;
        int n;

        if (chunk > chunk_max) {
            chunk = chunk_max;
        }
        memset(&pkt, 0, sizeof(pkt));
        pkt.rtp.v = RTP_VERSION;
        pkt.rtp.pt = s->payload_type;
        pkt.rtp.seq = (uint16_t)(s->seq++);
        pkt.rtp.timestamp = timestamp;
        pkt.rtp.ssrc = s->ssrc;
        pkt.rtp.m = (off + chunk >= len) ? 1 : 0;
        pkt.payload = ps + off;
        pkt.payloadlen = (int)chunk;

        n = rtp_packet_serialize(&pkt, s->packet, (int)s->mtu);
        if (n <= 0) {
            return -1;
        }
        if (s->write(s->user, s->packet, (size_t)n) != 0) {
            return -1;
        }
        off += chunk;
    }
    return 0;
}
