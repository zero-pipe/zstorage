/**
 * @file test_zm_ts_file.c
 * @brief MPEG-TS file -> zm_mpeg_reader (real H264/AAC).
 *
 * Skips cleanly when tests/fixtures/h264_aac.ts is absent.
 */
#include "zm/format/mpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZCS_FIXTURE_DIR
#define ZCS_FIXTURE_DIR "tests/fixtures"
#endif

static int g_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                               \
        }                                                                               \
    } while (0)

int main(void)
{
    const char *path = getenv("ZCS_FIXTURE");
    zm_reader_cfg cfg;
    zm_reader *r;
    FILE *fp;
    uint8_t buf[65536];
    int video = 0;
    int audio = 0;
    int keyframes = 0;
    zm_packet pkt;
    const zm_track *tracks;
    size_t track_count;

    if (!path || !*path) {
        path = ZCS_FIXTURE_DIR "/h264_aac.ts";
    }
    fp = fopen(path, "rb");
    if (!fp) {
        printf("test_zm_ts_file: skip (no %s)\n", path);
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.format = ZM_FMT_MPEG_TS;
    cfg.mode = ZM_READ_ES;
    r = zm_mpeg_reader_open(&cfg);
    CHECK(r != NULL);
    if (!r) {
        fclose(fp);
        return 1;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) {
            break;
        }
        CHECK(zm_reader_input(r, buf, n) == ZM_OK);
        while (zm_reader_read(r, &pkt) == 1) {
            if (pkt.kind == ZM_KIND_VIDEO) {
                ++video;
                CHECK(pkt.es_format == ZM_ES_ANNEXB);
            } else if (pkt.kind == ZM_KIND_AUDIO) {
                ++audio;
                CHECK(pkt.es_format == ZM_ES_RAW);
            }
            if (pkt.flags & ZM_PKT_KEY) {
                ++keyframes;
            }
            zm_packet_clear(&pkt);
        }
    }
    fclose(fp);

    CHECK(video > 0);
    CHECK(audio > 0);
    CHECK(keyframes > 0);
    tracks = zm_reader_tracks(r, &track_count);
    CHECK(track_count == 2);
    if (tracks) {
        CHECK(tracks[0].codec == ZM_CODEC_H264);
        CHECK(tracks[1].codec == ZM_CODEC_AAC);
    }
    zm_reader_close(r);

    if (g_failures) {
        fprintf(stderr, "test_zm_ts_file: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_ts_file ok (video=%d audio=%d keyframes=%d)\n", video, audio, keyframes);
    return 0;
}
