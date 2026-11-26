/**
 * @file test_zm_rtp.c
 * @brief RTP 接口往返：G711A payload pack -> RTP packet -> unpack -> zm_packet。
 */
#include "zm/transport/rtp.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                               \
        }                                                                               \
    } while (0)

typedef struct capture {
    uint8_t data[4096];
    size_t len;
} capture;

static int capture_write(void *ctx, const void *data, size_t len)
{
    capture *c = (capture *)ctx;

    if (c->len + len > sizeof(c->data)) {
        return -1;
    }
    memcpy(c->data + c->len, data, len);
    c->len += len;
    return (int)len;
}

static zm_sink make_sink(capture *c)
{
    zm_sink s;
    memset(&s, 0, sizeof(s));
    s.ctx = c;
    s.write = capture_write;
    return s;
}

static void test_g711a_roundtrip(void)
{
    uint8_t frame0[160];
    uint8_t frame1[160];
    zm_rtp_writer_cfg wcfg;
    zm_rtp_reader_cfg rcfg;
    capture cap;
    zm_sink sink;
    zm_writer *w;
    zm_reader *r;
    zm_packet pkt;
    const zm_track *tracks;
    size_t track_count;
    size_t i;

    for (i = 0; i < sizeof(frame0); ++i) {
        frame0[i] = (uint8_t)i;
        frame1[i] = (uint8_t)(0x80 + i);
    }

    memset(&cap, 0, sizeof(cap));
    memset(&wcfg, 0, sizeof(wcfg));
    sink = make_sink(&cap);
    wcfg.codec = ZM_CODEC_G711A;
    wcfg.payload_type = 8;
    wcfg.encoding = "PCMA";
    wcfg.clock_hz = 8000;
    wcfg.seq = 1;
    wcfg.ssrc = 0x12345678u;

    w = zm_rtp_writer_open(&sink, &wcfg);
    CHECK(w != NULL);
    if (!w) {
        return;
    }

    zm_packet_init(&pkt);
    pkt.mem = zm_mem_borrow(frame0, sizeof(frame0));
    pkt.codec = ZM_CODEC_G711A;
    pkt.kind = ZM_KIND_AUDIO;
    pkt.dts_ms = 0;
    pkt.pts_ms = 0;
    pkt.es_format = ZM_ES_RAW;
    CHECK(zm_writer_write(w, &pkt) == ZM_OK);
    CHECK(cap.len > sizeof(frame0)); /* RTP header + payload */

    {
        size_t first_len = cap.len;
        zm_packet_init(&pkt);
        pkt.mem = zm_mem_borrow(frame1, sizeof(frame1));
        pkt.codec = ZM_CODEC_G711A;
        pkt.kind = ZM_KIND_AUDIO;
        pkt.dts_ms = 20;
        pkt.pts_ms = 20;
        pkt.es_format = ZM_ES_RAW;
        CHECK(zm_writer_write(w, &pkt) == ZM_OK);
        CHECK(cap.len > first_len);
    }
    zm_writer_close(w);

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.codec = ZM_CODEC_G711A;
    rcfg.payload_type = 8;
    rcfg.encoding = "PCMA";
    rcfg.clock_hz = 8000;
    rcfg.jitter_ms = 0;
    r = zm_rtp_reader_open(&rcfg);
    CHECK(r != NULL);
    if (!r) {
        return;
    }

    tracks = zm_reader_tracks(r, &track_count);
    CHECK(track_count == 1);
    CHECK(tracks != NULL);
    if (tracks) {
        CHECK(tracks[0].codec == ZM_CODEC_G711A);
        CHECK(tracks[0].kind == ZM_KIND_AUDIO);
        CHECK(tracks[0].clock_hz == 8000);
    }

    /* Feed each RTP packet; the capture holds two packets back to back.
     * Parse RTP lengths using the seq/payload size: G711 packs one packet per frame. */
    {
        const uint8_t *p = cap.data;
        size_t left = cap.len;
        int got = 0;
        while (left > 12) {
            int payload_len = 160;
            size_t pkt_len = 12 + (size_t)payload_len;
            if (pkt_len > left) {
                break;
            }
            /* CSRC/extension not used by the packer for G711. */
            CHECK(zm_rtp_reader_input_rtp(r, p, pkt_len) == ZM_OK);
            p += pkt_len;
            left -= pkt_len;
            zm_packet_init(&pkt);
            CHECK(zm_reader_read(r, &pkt) == 1);
            CHECK(pkt.codec == ZM_CODEC_G711A);
            CHECK(pkt.kind == ZM_KIND_AUDIO);
            CHECK(pkt.mem.len == payload_len);
            if (got == 0) {
                CHECK(pkt.dts_ms == 0);
                CHECK(memcmp(pkt.mem.data, frame0, sizeof(frame0)) == 0);
            } else {
                CHECK(pkt.dts_ms == 20);
                CHECK(memcmp(pkt.mem.data, frame1, sizeof(frame1)) == 0);
            }
            zm_packet_clear(&pkt);
            got++;
        }
        CHECK(got == 2);
    }

    zm_reader_close(r);
}

int main(void)
{
    test_g711a_roundtrip();

    if (g_failures) {
        fprintf(stderr, "test_zm_rtp: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_rtp ok\n");
    return 0;
}
