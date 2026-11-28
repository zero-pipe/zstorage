/**
 * @file test_bridge.c
 * @brief ZM <-> ZCS 桥接单测：zn_buf 零拷贝、frame/gop slot 映射、codec id。
 */
#include "bridge/zm_zcs_bridge.h"

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

static void test_zn_buf_wrap(void)
{
    zn_buf *b = zn_buf_alloc(16);
    zm_mem m;
    zm_mem m2;
    uint8_t *w;

    CHECK(b != NULL);
    if (!b) {
        return;
    }
    w = (uint8_t *)zn_buf_data(b);
    memcpy(w, "abcd", 4);
    zn_buf_set_len(b, 4);

    m = zcs_zm_mem_from_zn(b);
    CHECK(m.owner == b);
    CHECK(m.data == (const uint8_t *)zn_buf_data(b));
    CHECK(m.len == 4);
    CHECK(m.cap >= 16);
    CHECK(zn_buf_refcnt(b) == 1);

    m2 = m;
    zm_mem_ref(&m2);
    CHECK(zn_buf_refcnt(b) == 2);
    zm_mem_unref(&m2);
    CHECK(zn_buf_refcnt(b) == 1);
    CHECK(m2.owner == NULL);

    zm_mem_unref(&m);
    CHECK(m.owner == NULL);
    /* b is now freed; do not touch it. */
}

static void test_frame_roundtrip(void)
{
    uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    zcs_frame f;
    zcs_frame back;
    zm_packet p;

    zcs_frame_init(&f);
    CHECK(zcs_frame_assign(&f, payload, sizeof(payload), 0) == ZN_OK);
    f.codec = ZCS_CODEC_H264;
    f.track = ZCS_TRACK_VIDEO;
    f.keyframe = 1;
    f.config_frame = 0;
    f.drop_able = 0;
    f.dts_ms = 100;
    f.pts_ms = 120;

    zcs_zm_packet_from_frame(&f, &p);
    CHECK(p.mem.data == payload);
    CHECK(p.mem.len == sizeof(payload));
    CHECK(p.codec == ZM_CODEC_H264);
    CHECK(p.kind == ZM_KIND_VIDEO);
    CHECK(p.dts_ms == 100);
    CHECK(p.pts_ms == 120);
    CHECK((p.flags & ZM_PKT_KEY) != 0);
    CHECK(p.es_format == ZM_ES_ANNEXB);

    CHECK(zcs_zm_frame_from_packet(&p, &back) == ZM_OK);
    CHECK(back.data == payload);
    CHECK(back.size == sizeof(payload));
    CHECK(back.capacity == p.mem.cap);
    CHECK(back.owned == 0);
    CHECK(back.codec == ZCS_CODEC_H264);
    CHECK(back.track == ZCS_TRACK_VIDEO);
    CHECK(back.keyframe == 1);
    CHECK(back.dts_ms == 100);
    CHECK(back.pts_ms == 120);

    zm_packet_clear(&p);
    zcs_frame_clear(&f);
    zcs_frame_clear(&back);
}

static void test_gop_slot(void)
{
    uint8_t payload[4] = {9, 8, 7, 6};
    zcs_gop_slot slot;
    zm_packet p;

    memset(&slot, 0, sizeof(slot));
    slot.codec = ZCS_CODEC_AAC;
    slot.track = ZCS_TRACK_AUDIO;
    slot.dts_ms = 200;
    slot.pts_ms = 0; /* 0 means "same as dts" */
    slot.config_frame = 1;
    slot.data = payload;
    slot.len = sizeof(payload);

    zcs_zm_packet_from_gop_slot(&slot, &p);
    CHECK(p.mem.data == payload);
    CHECK(p.mem.len == sizeof(payload));
    CHECK(p.codec == ZM_CODEC_AAC);
    CHECK(p.kind == ZM_KIND_AUDIO);
    CHECK(p.dts_ms == 200);
    CHECK(p.pts_ms == 200);
    CHECK((p.flags & ZM_PKT_CONFIG) != 0);
    CHECK(p.es_format == ZM_ES_RAW);
}

static void test_codec_map(void)
{
    CHECK(zcs_zm_codec_from_zcs(ZCS_CODEC_H264) == ZM_CODEC_H264);
    CHECK(zcs_zm_codec_from_zcs(ZCS_CODEC_AAC) == ZM_CODEC_AAC);
    CHECK(zcs_zm_codec_from_zcs(ZCS_CODEC_INVALID) == ZM_CODEC_INVALID);
    CHECK(zcs_zm_codec_to_zcs(ZM_CODEC_H265) == ZCS_CODEC_H265);
    CHECK(zcs_zm_codec_to_zcs(ZM_CODEC_AVS3) == ZCS_CODEC_INVALID);
}

static void test_media_track(void)
{
    zcs_media_track in;
    zm_track t;

    memset(&in, 0, sizeof(in));
    in.type = ZCS_TRACK_AUDIO;
    in.codec = ZCS_CODEC_AAC;
    in.sample_rate = 44100;
    in.channels = 2;

    zcs_zm_track_from_media_track(&in, &t);
    CHECK(t.codec == ZM_CODEC_AAC);
    CHECK(t.kind == ZM_KIND_AUDIO);
    CHECK(t.clock_hz == 44100);
    CHECK(t.sample_rate == 44100);
    CHECK(t.channels == 2);
    CHECK(t.timebase.num == 1 && t.timebase.den == 1000);
    zm_track_clear(&t);
}

int main(void)
{
    test_zn_buf_wrap();
    test_frame_roundtrip();
    test_gop_slot();
    test_codec_map();
    test_media_track();

    if (g_failures) {
        fprintf(stderr, "test_bridge: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_bridge ok\n");
    return 0;
}
