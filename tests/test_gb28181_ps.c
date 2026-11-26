/**
 * @file test_gb28181_ps.c
 * @brief GB28181 媒体封装：ES → PS → RTP/PS 分片/重组。
 */
#include "bridge/zm_ps_muxer.h"
#include "bridge/zm_rtp_ps.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t data[65536];
    size_t len;
} cap_buf;

typedef struct {
    uint8_t pkt[128][1600];
    size_t len[128];
    int count;
} rtp_cap;

static int ps_cb(void *user, int stream, const uint8_t *ps, size_t len)
{
    cap_buf *c = (cap_buf *)user;

    (void)stream;
    if (c->len + len > sizeof(c->data)) {
        return -1;
    }
    memcpy(c->data + c->len, ps, len);
    c->len += len;
    return 0;
}

static int rtp_cb(void *user, const uint8_t *rtp, size_t len)
{
    rtp_cap *c = (rtp_cap *)user;

    if (c->count >= 128 || len > sizeof(c->pkt[0])) {
        return -1;
    }
    memcpy(c->pkt[c->count], rtp, len);
    c->len[c->count] = len;
    c->count++;
    return 0;
}

static const uint8_t k_fake_h264[] = {
    0x00, 0x00, 0x00, 0x01, 0x09, 0xE0, /* AUD */
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x21, 0x00, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

int main(void)
{
    zms_zm_ps_muxer_opts mopts;
    zms_zm_ps_muxer *mux;
    zms_zm_rtp_ps_opts ropts;
    zms_zm_rtp_ps *rtp;
    cap_buf ps;
    rtp_cap rc;
    uint8_t frame[900];
    int vid;
    size_t off = 0;
    int i;

    memset(&ps, 0, sizeof(ps));
    memset(&rc, 0, sizeof(rc));
    memcpy(frame, k_fake_h264, sizeof(k_fake_h264));
    memset(frame + sizeof(k_fake_h264), 0x11, sizeof(frame) - sizeof(k_fake_h264));

    mopts.write = ps_cb;
    mopts.user = &ps;
    mux = zms_zm_ps_muxer_create(&mopts);
    CHECK(mux != NULL);
    vid = zms_zm_ps_muxer_add_video(mux, ZMS_CODEC_H264);
    CHECK(vid > 0);

    /* 90000 = 1 秒；IDR 触发 PSM。 */
    CHECK(zms_zm_ps_muxer_input_video(mux, vid, frame, sizeof(frame), 1, 90000, 90000) == 0);
    CHECK(ps.len > 16);

    /* PS pack header + PSM + video PES。 */
    CHECK(ps.data[0] == 0x00 && ps.data[1] == 0x00 && ps.data[2] == 0x01 && ps.data[3] == 0xBA);
    {
        int has_psm = 0;
        int has_pes = 0;
        for (i = 0; i + 4 <= (int)ps.len; ++i) {
            if (ps.data[i] == 0x00 && ps.data[i + 1] == 0x00 && ps.data[i + 2] == 0x01) {
                if (ps.data[i + 3] == 0xBC) {
                    has_psm = 1;
                }
                if (ps.data[i + 3] == 0xE0) {
                    has_pes = 1;
                }
            }
        }
        CHECK(has_psm);
        CHECK(has_pes);
    }

    /* 分片：MTU 300 保证多包。 */
    ropts.payload_type = 96;
    ropts.ssrc = 0x01020304u;
    ropts.seq = 7;
    ropts.mtu = 300;
    ropts.write = rtp_cb;
    ropts.user = &rc;
    rtp = zms_zm_rtp_ps_create(&ropts);
    CHECK(rtp != NULL);
    CHECK(zms_zm_rtp_ps_input(rtp, ps.data, ps.len, 90000) == 0);
    CHECK(rc.count >= 2);

    /* 头字段与 marker 位；重组后与 PS 完全一致。 */
    for (i = 0; i < rc.count; ++i) {
        const uint8_t *p = rc.pkt[i];
        size_t plen = rc.len[i] - 12;
        CHECK((p[0] >> 6) == 2);
        CHECK((p[1] & 0x7F) == 96);
        CHECK(((p[1] >> 7) & 1) == (i == rc.count - 1 ? 1 : 0));
        CHECK(off + plen <= ps.len);
        CHECK(memcmp(p + 12, ps.data + off, plen) == 0);
        off += plen;
    }
    CHECK(off == ps.len);
    CHECK(((rc.pkt[0][2] << 8) | rc.pkt[0][3]) == 7);
    CHECK(((uint32_t)rc.pkt[0][8] << 24 | (uint32_t)rc.pkt[0][9] << 16 |
           (uint32_t)rc.pkt[0][10] << 8 | rc.pkt[0][11]) == 0x01020304u);

    zms_zm_rtp_ps_destroy(rtp);
    zms_zm_ps_muxer_destroy(mux);
    printf("test_gb28181_ps ok\n");
    return 0;
}
