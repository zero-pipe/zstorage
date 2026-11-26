/**
 * @file test_zm_hls.c
 * @brief HLS(TS) packager: G711A frames -> segments + playlist.
 */
#include "zm/packager/packager.h"

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
    int segments;
    int playlists;
    size_t last_segment_bytes;
    int64_t last_duration;
    char playlist[8192];
} capture;

static int on_init(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    (void)name;
    (void)data;
    (void)len;
    return 0;
}

static int on_segment(void *ctx, const char *name, const void *data, size_t len, int64_t pts,
                      int64_t duration, int discontinuity)
{
    capture *c = (capture *)ctx;

    (void)name;
    (void)pts;
    (void)discontinuity;
    if (!data || len == 0 || len % 188 != 0) {
        fprintf(stderr, "FAIL segment len=%zu\n", len);
        g_failures++;
    }
    c->segments++;
    c->last_segment_bytes = len;
    c->last_duration = duration;
    return 0;
}

static int on_playlist(void *ctx, const char *name, const char *text, size_t len)
{
    capture *c = (capture *)ctx;

    (void)name;
    if (len >= sizeof(c->playlist)) {
        len = sizeof(c->playlist) - 1;
    }
    memcpy(c->playlist, text, len);
    c->playlist[len] = '\0';
    c->playlists++;
    return 0;
}

int main(void)
{
    zm_track track;
    zm_packager_cfg cfg;
    zm_packager_cb cb;
    zm_packager *p;
    capture cap;
    uint8_t frame[160];
    size_t i;
    int f;

    memset(&cap, 0, sizeof(cap));
    memset(&cb, 0, sizeof(cb));
    cb.on_init = on_init;
    cb.on_segment = on_segment;
    cb.on_playlist = on_playlist;

    zm_track_init(&track);
    track.track_id = 1;
    track.codec = ZM_CODEC_G711A;
    track.kind = ZM_KIND_AUDIO;
    track.clock_hz = 8000;
    track.sample_rate = 8000;
    track.channels = 1;
    track.timebase.num = 1;
    track.timebase.den = 1000;

    memset(&cfg, 0, sizeof(cfg));
    cfg.segment_ms = 100;
    cfg.live = 0;

    p = zm_hls_packager_open(&track, 1, &cfg, &cb, &cap);
    CHECK(p != NULL);
    if (!p) {
        zm_track_clear(&track);
        return 1;
    }

    for (f = 0; f < 10; ++f) {
        zm_packet pkt;
        for (i = 0; i < sizeof(frame); ++i) {
            frame[i] = (uint8_t)(f * 16 + i);
        }
        zm_packet_init(&pkt);
        pkt.mem = zm_mem_borrow(frame, sizeof(frame));
        pkt.codec = ZM_CODEC_G711A;
        pkt.kind = ZM_KIND_AUDIO;
        pkt.track_id = 1;
        pkt.dts_ms = (int64_t)f * 20;
        pkt.pts_ms = pkt.dts_ms;
        pkt.es_format = ZM_ES_RAW;
        CHECK(zm_packager_write(p, &pkt) == ZM_OK);
    }
    CHECK(zm_packager_flush(p, 1) == ZM_OK);
    zm_packager_close(p);
    zm_track_clear(&track);

    CHECK(cap.segments >= 1);
    CHECK(cap.playlists >= 1);
    CHECK(cap.last_segment_bytes > 0);
    CHECK(cap.last_duration > 0);
    CHECK(strstr(cap.playlist, "#EXTM3U") != NULL);
    CHECK(strstr(cap.playlist, ".ts") != NULL);

    /* live 模式：hls_m3u8_create 需要窗口大小 >= HLS_LIVE_NUM */
    {
        capture lcap;
        memset(&lcap, 0, sizeof(lcap));
        zm_track_init(&track);
        track.track_id = 1;
        track.codec = ZM_CODEC_G711A;
        track.kind = ZM_KIND_AUDIO;
        track.clock_hz = 8000;
        track.sample_rate = 8000;
        track.channels = 1;
        track.timebase.num = 1;
        track.timebase.den = 1000;
        memset(&cfg, 0, sizeof(cfg));
        cfg.segment_ms = 100;
        cfg.live = 1;
        cfg.window_segments = 3;
        p = zm_hls_packager_open(&track, 1, &cfg, &cb, &lcap);
        CHECK(p != NULL);
        if (p) {
            for (f = 0; f < 10; ++f) {
                zm_packet pkt;
                zm_packet_init(&pkt);
                pkt.mem = zm_mem_borrow(frame, sizeof(frame));
                pkt.codec = ZM_CODEC_G711A;
                pkt.kind = ZM_KIND_AUDIO;
                pkt.track_id = 1;
                pkt.dts_ms = (int64_t)f * 20;
                pkt.pts_ms = pkt.dts_ms;
                pkt.es_format = ZM_ES_RAW;
                CHECK(zm_packager_write(p, &pkt) == ZM_OK);
            }
            CHECK(zm_packager_flush(p, 0) == ZM_OK);
            zm_packager_close(p);
        }
        CHECK(lcap.segments >= 1);
        CHECK(lcap.playlists >= 1);
        zm_track_clear(&track);
    }

    if (g_failures) {
        fprintf(stderr, "test_zm_hls: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_hls ok (segments=%d)\n", cap.segments);
    return 0;
}
