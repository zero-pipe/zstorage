/**
 * @file test_zm_mpeg.c
 * @brief MPEG-TS / PS 接口往返：G711A -> mux -> demux -> zm_packet。
 */
#include "zm/format/mpeg.h"

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
    uint8_t data[32768];
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
    return 0;
}

static zm_sink make_sink(capture *c)
{
    zm_sink s;
    memset(&s, 0, sizeof(s));
    s.ctx = c;
    s.write = capture_write;
    return s;
}

static zm_track make_g711_track(void)
{
    zm_track t;
    zm_track_init(&t);
    t.track_id = 1;
    t.codec = ZM_CODEC_G711A;
    t.kind = ZM_KIND_AUDIO;
    t.clock_hz = 8000;
    t.sample_rate = 8000;
    t.channels = 1;
    t.timebase.num = 1;
    t.timebase.den = 1000;
    return t;
}

static void run_roundtrip(zm_format format, const char *name)
{
    uint8_t frame0[160];
    uint8_t frame1[160];
    zm_track track = make_g711_track();
    zm_writer_cfg wcfg;
    zm_reader_cfg rcfg;
    capture cap;
    zm_sink sink;
    zm_writer *w;
    zm_reader *r;
    zm_packet pkt;
    const zm_track *tracks;
    size_t track_count;
    size_t i;
    int got = 0;

    for (i = 0; i < sizeof(frame0); ++i) {
        frame0[i] = (uint8_t)i;
        frame1[i] = (uint8_t)(0x40 + i);
    }

    memset(&cap, 0, sizeof(cap));
    memset(&wcfg, 0, sizeof(wcfg));
    sink = make_sink(&cap);
    wcfg.format = format;
    wcfg.mode = ZM_WRITE_ES;
    wcfg.tracks = &track;
    wcfg.track_count = 1;

    w = zm_mpeg_writer_open(&sink, &wcfg);
    CHECK(w != NULL);
    if (!w) {
        zm_track_clear(&track);
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

    zm_packet_init(&pkt);
    pkt.mem = zm_mem_borrow(frame1, sizeof(frame1));
    pkt.codec = ZM_CODEC_G711A;
    pkt.kind = ZM_KIND_AUDIO;
    pkt.dts_ms = 20;
    pkt.pts_ms = 20;
    pkt.es_format = ZM_ES_RAW;
    CHECK(zm_writer_write(w, &pkt) == ZM_OK);
    zm_writer_close(w);
    CHECK(cap.len > 0);

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = format;
    rcfg.mode = ZM_READ_ES;
    r = zm_mpeg_reader_open(&rcfg);
    CHECK(r != NULL);
    if (!r) {
        zm_track_clear(&track);
        return;
    }

    CHECK(zm_reader_input(r, cap.data, cap.len) == ZM_OK);

    while (zm_reader_read(r, &pkt) == 1) {
        CHECK(pkt.codec == ZM_CODEC_G711A);
        CHECK(pkt.kind == ZM_KIND_AUDIO);
        CHECK(pkt.mem.len == 160);
        if (got == 0) {
            CHECK(pkt.dts_ms == 0);
            CHECK(memcmp(pkt.mem.data, frame0, sizeof(frame0)) == 0);
        } else if (got == 1) {
            CHECK(pkt.dts_ms == 20);
            CHECK(memcmp(pkt.mem.data, frame1, sizeof(frame1)) == 0);
        }
        zm_packet_clear(&pkt);
        ++got;
    }
    CHECK(got == 2);

    tracks = zm_reader_tracks(r, &track_count);
    CHECK(track_count == 2);
    CHECK(tracks != NULL);
    if (tracks) {
        CHECK(tracks[1].codec == ZM_CODEC_G711A);
        CHECK(tracks[1].kind == ZM_KIND_AUDIO);
        CHECK(tracks[1].clock_hz == 8000);
    }

    zm_reader_close(r);
    zm_track_clear(&track);
    if (g_failures) {
        fprintf(stderr, "  (in %s round trip)\n", name);
    }
}

int main(void)
{
    run_roundtrip(ZM_FMT_MPEG_TS, "TS");
    run_roundtrip(ZM_FMT_MPEG_PS, "PS");

    if (g_failures) {
        fprintf(stderr, "test_zm_mpeg: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_mpeg ok (TS + PS)\n");
    return 0;
}
