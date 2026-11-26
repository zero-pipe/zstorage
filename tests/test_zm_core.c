/**
 * @file test_zm_core.c
 * @brief ZM 共用媒体模型单测：mem / packet / track / codec registry。
 */
#include "zm/zm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                               \
        }                                                                               \
    } while (0)

typedef struct test_owner {
    int refs;
    int releases;
} test_owner;

static void test_owner_ref(void *p)
{
    ((test_owner *)p)->refs++;
}

static void test_owner_release(void *p)
{
    ((test_owner *)p)->releases++;
}

static void test_mem(void)
{
    static const char k_lit[] = "abcd";
    zm_mem m;
    zm_mem d;
    test_owner owner = {0, 0};
    uint8_t raw[4] = {1, 2, 3, 4};

    m = zm_mem_borrow(k_lit, 4);
    CHECK(m.owner == NULL);
    CHECK(m.data == (const uint8_t *)k_lit);
    CHECK(m.len == 4);
    zm_mem_unref(&m);
    CHECK(m.data == NULL && m.len == 0 && m.owner == NULL);

    CHECK(zm_mem_alloc(16, &m) == ZM_OK);
    CHECK(m.data != NULL);
    CHECK(m.cap >= 16);
    CHECK(m.len == 0);
    memcpy(m.data, "abcd", 4);
    zm_mem_set_len(&m, 4);
    CHECK(m.len == 4);
    CHECK(memcmp(m.data, "abcd", 4) == 0);

    CHECK(zm_mem_reserve(&m, 64) == ZM_OK);
    CHECK(m.cap >= 64);
    CHECK(m.len == 4);
    CHECK(memcmp(m.data, "abcd", 4) == 0);
    zm_mem_unref(&m);

    CHECK(zm_mem_dup("xyz", 3, &d) == ZM_OK);
    CHECK(d.len == 3);
    CHECK(memcmp(d.data, "xyz", 3) == 0);
    zm_mem_unref(&d);

    m = zm_mem_wrap(&owner, test_owner_ref, test_owner_release, raw, sizeof(raw), sizeof(raw));
    CHECK(m.owner == &owner);
    CHECK(m.len == sizeof(raw));
    zm_mem_ref(&m);
    CHECK(owner.refs == 1);
    zm_mem_unref(&m);
    CHECK(owner.releases == 1);
    CHECK(m.owner == NULL);
}

static void test_packet(void)
{
    zm_packet p;
    zm_raw_unit u;

    zm_packet_init(&p);
    CHECK(p.mem.data == NULL);
    CHECK(p.codec == ZM_CODEC_INVALID);
    p.codec = ZM_CODEC_H264;
    p.kind = ZM_KIND_VIDEO;
    p.dts_ms = 100;
    p.pts_ms = 120;
    p.flags = ZM_PKT_KEY;
    zm_packet_clear(&p);
    CHECK(p.codec == ZM_CODEC_INVALID);
    CHECK(p.flags == 0);

    zm_raw_unit_init(&u);
    CHECK(u.kind == 0);
    CHECK(u.body.data == NULL);
    zm_raw_unit_clear(&u);
}

static void test_track(void)
{
    static const uint8_t k_avcc[] = {0x01, 0x42, 0x00, 0x1e, 0xff};
    zm_track t;
    zm_track u;
    zm_config copy;

    zm_track_init(&t);
    CHECK(t.codec == ZM_CODEC_INVALID);
    t.codec = ZM_CODEC_H264;
    t.kind = ZM_KIND_VIDEO;
    t.clock_hz = 90000;
    CHECK(zm_track_set_config(&t, ZM_CONFIG_AVCC, k_avcc, sizeof(k_avcc)) == ZM_OK);
    CHECK(t.config.fmt == ZM_CONFIG_AVCC);
    CHECK(t.config.len == sizeof(k_avcc));
    CHECK(memcmp(t.config.data, k_avcc, sizeof(k_avcc)) == 0);

    zm_track_init(&u);
    memset(&copy, 0, sizeof(copy));
    CHECK(zm_config_copy(&copy, &t.config) == ZM_OK);
    CHECK(copy.len == sizeof(k_avcc));
    CHECK(memcmp(copy.data, k_avcc, sizeof(k_avcc)) == 0);
    free(copy.data);

    CHECK(zm_track_set_config(&t, ZM_CONFIG_NONE, NULL, 0) == ZM_OK);
    CHECK(t.config.data == NULL && t.config.len == 0 && t.config.fmt == ZM_CONFIG_NONE);
    zm_track_clear(&t);
}

static void test_codec(void)
{
    const zm_codec_desc *d = zm_codec_get(ZM_CODEC_H264);

    CHECK(d != NULL);
    if (d) {
        CHECK(strcmp(d->name, "H264") == 0);
        CHECK(d->kind == ZM_KIND_VIDEO);
        CHECK(d->clock_hz == 90000);
        CHECK(d->map.flv_video == 7);
        CHECK(d->map.psi == 0x1b);
        CHECK(d->map.mov_object == 0x21);
        CHECK(d->map.mkv_codec == 9);
        CHECK(d->map.rtp_payload == 98);
    }
    CHECK(zm_codec_get(ZM_CODEC_INVALID) == NULL);
    CHECK(strcmp(zm_codec_name(ZM_CODEC_AAC), "AAC") == 0);
    CHECK(zm_codec_kind(ZM_CODEC_AAC) == ZM_KIND_AUDIO);
    CHECK(zm_codec_clock_hz(ZM_CODEC_OPUS) == 48000);
    CHECK(zm_codec_count() == 14);

    CHECK(zm_codec_from_name("H264") == ZM_CODEC_H264);
    CHECK(zm_codec_from_name("H.264") == ZM_CODEC_H264);
    CHECK(zm_codec_from_name("AVC") == ZM_CODEC_H264);
    CHECK(zm_codec_from_name("HEVC") == ZM_CODEC_H265);
    CHECK(zm_codec_from_name("mpeg4-generic") == ZM_CODEC_AAC);
    CHECK(zm_codec_from_name("not-a-codec") == ZM_CODEC_INVALID);

    CHECK(zm_codec_from_flv(7) == ZM_CODEC_H264);
    CHECK(zm_codec_from_flv(12) == ZM_CODEC_H265);
    CHECK(zm_codec_from_flv(160) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_flv(208) == ZM_CODEC_OPUS);
    CHECK(zm_codec_from_flv(999) == ZM_CODEC_INVALID);

    CHECK(zm_codec_from_psi(0x1b) == ZM_CODEC_H264);
    CHECK(zm_codec_from_psi(0x25) == ZM_CODEC_H265);
    CHECK(zm_codec_from_psi(0x34) == ZM_CODEC_H266);
    CHECK(zm_codec_from_psi(0x0f) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_psi(0x11) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_psi(0x7f) == ZM_CODEC_INVALID);

    CHECK(zm_codec_from_mov(0x21) == ZM_CODEC_H264);
    CHECK(zm_codec_from_mov(0x23) == ZM_CODEC_H265);
    CHECK(zm_codec_from_mov(0x40) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_mov(0x67) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_mov(0x00) == ZM_CODEC_INVALID);

    CHECK(zm_codec_from_mkv(9) == ZM_CODEC_H264);
    CHECK(zm_codec_from_mkv(10) == ZM_CODEC_H265);
    CHECK(zm_codec_from_mkv(24) == ZM_CODEC_H266);
    CHECK(zm_codec_from_mkv(4117) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_mkv(1) == ZM_CODEC_INVALID);

    CHECK(zm_codec_from_rtpmap("H264", 90000) == ZM_CODEC_H264);
    CHECK(zm_codec_from_rtpmap("HEVC", 90000) == ZM_CODEC_H265);
    CHECK(zm_codec_from_rtpmap("mpeg4-generic", 44100) == ZM_CODEC_AAC);
    CHECK(zm_codec_from_rtpmap("opus", 48000) == ZM_CODEC_OPUS);
    CHECK(zm_codec_from_rtpmap("PCMA", 8000) == ZM_CODEC_G711A);
    CHECK(zm_codec_from_rtpmap("unknown", 0) == ZM_CODEC_INVALID);
}

int main(void)
{
    test_mem();
    test_packet();
    test_track();
    test_codec();

    if (g_failures) {
        fprintf(stderr, "test_zm_core: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_core ok\n");
    return 0;
}
