/**
 * @file test_core_smoke.c
 * @brief P0 smoke test: frame lifecycle, codec registry, play-path suffix.
 *
 * Intentionally free of sockets and files so it can run anywhere.
 */
#include "zcs/core/frame.h"
#include "zcs/core/stream/url.h"
#include "zcs/media/codec/codec_id.h"

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

static void test_frame_lifecycle(void)
{
    static const uint8_t k_src[4] = {0x00, 0x00, 0x00, 0x01};
    zcs_frame f;

    zcs_frame_init(&f);
    CHECK(f.data == NULL);
    CHECK(f.size == 0);
    CHECK(f.capacity == 0);
    CHECK(f.owned == 0);

    CHECK(zcs_frame_reserve(&f, 16) == ZN_OK);
    CHECK(f.data != NULL);
    CHECK(f.capacity >= 16);
    CHECK(f.owned == 1);

    CHECK(zcs_frame_assign(&f, k_src, sizeof(k_src), 1) == ZN_OK);
    CHECK(f.size == sizeof(k_src));
    CHECK(f.data != NULL);
    CHECK(memcmp(f.data, k_src, sizeof(k_src)) == 0);
    CHECK(f.owned == 1);

    zcs_frame_clear(&f);
    CHECK(f.data == NULL);
    CHECK(f.size == 0);
    CHECK(f.capacity == 0);
    CHECK(f.owned == 0);

    /* Borrowed payload must not be freed by clear(). */
    CHECK(zcs_frame_assign(&f, k_src, sizeof(k_src), 0) == ZN_OK);
    CHECK(f.data == k_src);
    CHECK(f.owned == 0);
    zcs_frame_clear(&f);
    CHECK(f.data == NULL);
    CHECK(f.owned == 0);
}

static void test_codec_registry(void)
{
    const zcs_codec_meta *m;

    m = zcs_codec_meta_get(ZCS_CODEC_H264);
    CHECK(m != NULL);
    if (m) {
        CHECK(strcmp(m->name, "H264") == 0);
        CHECK(m->track == ZCS_TRACK_VIDEO);
    }

    CHECK(zcs_codec_track_type(ZCS_CODEC_AAC) == ZCS_TRACK_AUDIO);
    CHECK(zcs_codec_id_from_name("H265") == ZCS_CODEC_H265);
    CHECK(zcs_codec_id_from_name("mpeg4-generic") == ZCS_CODEC_AAC);
    CHECK(zcs_codec_id_from_name("not-a-codec") == ZCS_CODEC_INVALID);

    CHECK(zcs_codec_mpeg_psi(ZCS_CODEC_H264) == 0x1b);
    CHECK(zcs_codec_from_mpeg_psi(0x24) == ZCS_CODEC_H265);
    CHECK(zcs_codec_from_mpeg_psi(0x25) == ZCS_CODEC_H265);
    CHECK(zcs_codec_from_mpeg_psi(0x1c) == ZCS_CODEC_AAC);
    CHECK(zcs_codec_from_mpeg_psi(0x7f) == ZCS_CODEC_INVALID);
    CHECK(zcs_codec_count() == 10);
}

static void check_strip(const char *in, const char *want)
{
    char buf[128];

    snprintf(buf, sizeof(buf), "%s", in);
    zcs_media_path_strip_play_suffix(buf);
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "FAIL strip(\"%s\") = \"%s\", want \"%s\"\n", in, buf, want);
        g_failures++;
    }
}

static void test_play_path_strip(void)
{
    check_strip("live/test.m3u8", "live/test");
    check_strip("live/test.mpd", "live/test");
    check_strip("live/test.ts", "live/test");
    check_strip("live/test.mp4", "live/test");
    check_strip("live/test.mp4.flv", "live/test");
    /* Native VOD file names are deliberately preserved. */
    check_strip("live/test.flv", "live/test.flv");
    /* Numbered HLS segments are not treated as stream suffixes. */
    check_strip("live/test.5.ts", "live/test.5.ts");
}

int main(void)
{
    test_frame_lifecycle();
    test_codec_registry();
    test_play_path_strip();

    if (g_failures) {
        fprintf(stderr, "test_core_smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_core_smoke ok\n");
    return 0;
}
