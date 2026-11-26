/**
 * @file test_zm_hls_file.c
 * @brief HLS(TS) packager against a real H264/AAC MP4 fixture.
 *
 * Skips cleanly when tests/fixtures/h264_aac.mp4 is absent.
 */
#include "zm/format/mov.h"
#include "zm/packager/packager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZMS_FIXTURE_DIR
#define ZMS_FIXTURE_DIR "tests/fixtures"
#endif

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
    char playlist[16384];
} capture;

typedef struct file_io {
    FILE *fp;
} file_io;

static int file_read(void *ctx, void *buf, size_t len)
{
    file_io *f = (file_io *)ctx;
    return (int)fread(buf, 1, len, f->fp);
}

static int file_seek(void *ctx, int64_t off, int whence)
{
    file_io *f = (file_io *)ctx;
    return fseek(f->fp, (long)off, whence) == 0 ? 0 : -1;
}

static int64_t file_tell(void *ctx)
{
    file_io *f = (file_io *)ctx;
    return (int64_t)ftell(f->fp);
}

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
    (void)duration;
    (void)discontinuity;
    if (!data || len == 0 || len % 188 != 0) {
        fprintf(stderr, "FAIL bad segment len=%zu\n", len);
        g_failures++;
    }
    c->segments++;
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
    const char *path = getenv("ZMS_FIXTURE");
    file_io f;
    zm_io io;
    zm_reader *r;
    const zm_track *tracks;
    size_t track_count;
    zm_packager_cfg cfg;
    zm_packager_cb cb;
    zm_packager *p;
    capture cap;
    zm_packet pkt;
    int packets = 0;

    if (!path || !*path) {
        path = ZMS_FIXTURE_DIR "/h264_aac.mp4";
    }
    f.fp = fopen(path, "rb");
    if (!f.fp) {
        printf("test_zm_hls_file: skip (no %s)\n", path);
        return 0;
    }
    memset(&io, 0, sizeof(io));
    io.ctx = &f;
    io.read = file_read;
    io.seek = file_seek;
    io.tell = file_tell;

    r = zm_mov_reader_open(&io, NULL);
    CHECK(r != NULL);
    if (!r) {
        fclose(f.fp);
        return 1;
    }
    tracks = zm_reader_tracks(r, &track_count);
    CHECK(tracks != NULL && track_count > 0);

    memset(&cap, 0, sizeof(cap));
    memset(&cb, 0, sizeof(cb));
    cb.on_init = on_init;
    cb.on_segment = on_segment;
    cb.on_playlist = on_playlist;
    memset(&cfg, 0, sizeof(cfg));
    cfg.segment_ms = 1000;
    cfg.live = 0;

    p = zm_hls_packager_open(tracks, track_count, &cfg, &cb, &cap);
    CHECK(p != NULL);
    if (!p) {
        zm_reader_close(r);
        fclose(f.fp);
        return 1;
    }

    while (packets < 1000) {
        int rc;
        zm_packet_init(&pkt);
        rc = zm_reader_read(r, &pkt);
        if (rc <= 0) {
            break;
        }
        CHECK(zm_packager_write(p, &pkt) == ZM_OK);
        zm_packet_clear(&pkt);
        ++packets;
    }
    CHECK(zm_packager_flush(p, 1) == ZM_OK);
    zm_packager_close(p);
    zm_reader_close(r);
    fclose(f.fp);

    CHECK(packets > 0);
    CHECK(cap.segments >= 1);
    CHECK(cap.playlists >= 1);
    CHECK(strstr(cap.playlist, "#EXTM3U") != NULL);

    if (g_failures) {
        fprintf(stderr, "test_zm_hls_file: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_hls_file ok (packets=%d segments=%d)\n", packets, cap.segments);
    return 0;
}
