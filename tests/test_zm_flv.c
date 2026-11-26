/**
 * @file test_zm_flv.c
 * @brief FLV reader/writer 接口一致性：tag body 往返、AAC config 归一化、完整 tag 输出。
 */
#include "zm/format/flv.h"

#include "flv-header.h"

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

static int contains(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t needle_len)
{
    size_t i;
    if (needle_len == 0 || hay_len < needle_len) {
        return 0;
    }
    for (i = 0; i + needle_len <= hay_len; ++i) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static zm_track make_aac_track(const uint8_t *asc, size_t asc_len)
{
    zm_track t;
    zm_track_init(&t);
    t.track_id = 1;
    t.codec = ZM_CODEC_AAC;
    t.kind = ZM_KIND_AUDIO;
    t.clock_hz = 44100;
    t.sample_rate = 44100;
    t.channels = 2;
    t.timebase.num = 1;
    t.timebase.den = 1000;
    zm_track_set_config(&t, ZM_CONFIG_ASC, asc, asc_len);
    return t;
}

static void write_aac_packets(zm_writer *w, const uint8_t *asc, size_t asc_len)
{
    static const uint8_t k_frame[] = {0x21, 0x22, 0x23};
    zm_packet p;

    zm_packet_init(&p);
    p.mem = zm_mem_borrow(asc, asc_len);
    p.codec = ZM_CODEC_AAC;
    p.kind = ZM_KIND_AUDIO;
    p.flags = ZM_PKT_CONFIG;
    p.es_format = ZM_ES_RAW;
    CHECK(zm_writer_write(w, &p) == ZM_OK);

    zm_packet_init(&p);
    p.mem = zm_mem_borrow(k_frame, sizeof(k_frame));
    p.codec = ZM_CODEC_AAC;
    p.kind = ZM_KIND_AUDIO;
    p.dts_ms = 23;
    p.pts_ms = 23;
    p.es_format = ZM_ES_RAW;
    CHECK(zm_writer_write(w, &p) == ZM_OK);
}

static void test_tag_body_roundtrip(void)
{
    static const uint8_t k_asc[] = {0x12, 0x10};
    static const uint8_t k_seq_body[] = {0xaf, 0x00, 0x12, 0x10};
    static const uint8_t k_frame_body[] = {0xaf, 0x01, 0x21, 0x22, 0x23};
    zm_track track = make_aac_track(k_asc, sizeof(k_asc));
    zm_writer_cfg wcfg;
    zm_reader_cfg rcfg;
    capture cap;
    zm_sink sink;
    zm_writer *w;
    zm_reader *r;
    zm_packet p;
    const zm_track *tracks;
    size_t track_count;

    memset(&cap, 0, sizeof(cap));
    memset(&wcfg, 0, sizeof(wcfg));
    sink = make_sink(&cap);
    wcfg.format = ZM_FMT_FLV;
    wcfg.mode = ZM_WRITE_ES;
    wcfg.tracks = &track;
    wcfg.track_count = 1;
    wcfg.tag_body = 1;

    w = zm_flv_writer_open(&sink, &wcfg);
    CHECK(w != NULL);
    if (!w) {
        zm_track_clear(&track);
        return;
    }
    write_aac_packets(w, k_asc, sizeof(k_asc));
    CHECK(cap.len == sizeof(k_seq_body) + sizeof(k_frame_body));
    CHECK(memcmp(cap.data, k_seq_body, sizeof(k_seq_body)) == 0);
    CHECK(memcmp(cap.data + sizeof(k_seq_body), k_frame_body, sizeof(k_frame_body)) == 0);
    zm_writer_close(w);

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = ZM_FMT_FLV;
    rcfg.mode = ZM_READ_ES;
    rcfg.input_tag_body = 1;
    r = zm_flv_reader_open_push(&rcfg);
    CHECK(r != NULL);
    if (!r) {
        zm_track_clear(&track);
        return;
    }

    CHECK(zm_flv_reader_input_tag(r, 8, k_seq_body, sizeof(k_seq_body), 0) == ZM_OK);
    zm_packet_init(&p);
    CHECK(zm_reader_read(r, &p) == 1);
    CHECK(p.codec == ZM_CODEC_AAC);
    CHECK(p.kind == ZM_KIND_AUDIO);
    CHECK((p.flags & ZM_PKT_CONFIG) != 0);
    CHECK(p.mem.len == sizeof(k_asc));
    CHECK(memcmp(p.mem.data, k_asc, sizeof(k_asc)) == 0);
    zm_packet_clear(&p);

    tracks = zm_reader_tracks(r, &track_count);
    CHECK(track_count == 2);
    CHECK(tracks != NULL);
    if (tracks) {
        CHECK(tracks[1].codec == ZM_CODEC_AAC);
        CHECK(tracks[1].kind == ZM_KIND_AUDIO);
        CHECK(tracks[1].clock_hz == 44100);
        CHECK(tracks[1].sample_rate == 44100);
        CHECK(tracks[1].channels == 2);
        CHECK(tracks[1].config.fmt == ZM_CONFIG_ASC);
        CHECK(tracks[1].config.len == sizeof(k_asc));
        CHECK(memcmp(tracks[1].config.data, k_asc, sizeof(k_asc)) == 0);
    }

    CHECK(zm_flv_reader_input_tag(r, 8, k_frame_body, sizeof(k_frame_body), 23) == ZM_OK);
    zm_packet_init(&p);
    CHECK(zm_reader_read(r, &p) == 1);
    CHECK(p.codec == ZM_CODEC_AAC);
    CHECK((p.flags & ZM_PKT_CONFIG) == 0);
    CHECK(p.dts_ms == 23);
    CHECK(p.mem.len == 3);
    CHECK(memcmp(p.mem.data, k_frame_body + 2, 3) == 0);
    zm_packet_clear(&p);

    CHECK(zm_reader_read(r, &p) == 0);
    zm_reader_close(r);
    zm_track_clear(&track);
}

static void test_full_tag_writer(void)
{
    static const uint8_t k_asc[] = {0x12, 0x10};
    static const uint8_t k_seq_body[] = {0xaf, 0x00, 0x12, 0x10};
    zm_track track = make_aac_track(k_asc, sizeof(k_asc));
    zm_writer_cfg cfg;
    capture cap;
    zm_sink sink;
    zm_writer *w;
    struct flv_tag_header_t tag;

    memset(&cap, 0, sizeof(cap));
    memset(&cfg, 0, sizeof(cfg));
    sink = make_sink(&cap);
    cfg.format = ZM_FMT_FLV;
    cfg.mode = ZM_WRITE_ES;
    cfg.tracks = &track;
    cfg.track_count = 1;
    cfg.tag_body = 0;

    w = zm_flv_writer_open(&sink, &cfg);
    CHECK(w != NULL);
    if (!w) {
        zm_track_clear(&track);
        return;
    }
    CHECK(cap.len >= 13);
    CHECK(memcmp(cap.data, "FLV", 3) == 0);
    CHECK((cap.data[4] & 0x04) != 0); /* audio flag */

    write_aac_packets(w, k_asc, sizeof(k_asc));
    CHECK(contains(cap.data, cap.len, k_seq_body, sizeof(k_seq_body)));

    memset(&tag, 0, sizeof(tag));
    CHECK(flv_tag_header_read(&tag, cap.data + 13, cap.len - 13) > 0);
    CHECK(tag.type == 8);
    CHECK(tag.size == sizeof(k_seq_body));
    CHECK(tag.timestamp == 0);

    zm_writer_close(w);
    zm_track_clear(&track);
}

static void test_invalid_args(void)
{
    zm_reader_cfg rcfg;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = ZM_FMT_FLV;
    rcfg.mode = ZM_READ_RAW;
    CHECK(zm_flv_reader_open_push(&rcfg) == NULL);
    {
        zm_reader *r = zm_flv_reader_open_push(NULL);
        CHECK(r != NULL);
        if (r) {
            zm_reader_close(r);
        }
    }
    CHECK(zm_flv_writer_open(NULL, NULL) == NULL);
}

int main(void)
{
    test_tag_body_roundtrip();
    test_full_tag_writer();
    test_invalid_args();

    if (g_failures) {
        fprintf(stderr, "test_zm_flv: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_flv ok\n");
    return 0;
}
