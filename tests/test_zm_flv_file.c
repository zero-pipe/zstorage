/**
 * @file test_zm_flv_file.c
 * @brief FLV file -> zm_flv_reader -> zm_flv_writer -> FLV file round trip.
 *
 * Skips cleanly when tests/fixtures/h264_aac.flv is absent.
 */
#include "zm/format/flv.h"

#include "flv-reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

typedef struct capture {
    uint8_t *data;
    size_t len;
    size_t cap;
} capture;

static int capture_write(void *ctx, const void *data, size_t len)
{
    capture *c = (capture *)ctx;
    if (c->len + len > c->cap) {
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

typedef struct mem_io {
    const uint8_t *data;
    size_t len;
    size_t off;
} mem_io;

static int mem_read(void *param, void *buf, int len)
{
    mem_io *m = (mem_io *)param;
    size_t n = m->len - m->off;
    if (n > (size_t)len) {
        n = (size_t)len;
    }
    memcpy(buf, m->data + m->off, n);
    m->off += n;
    return (int)n;
}

typedef struct counts {
    int video;
    int audio;
    int video_cfg;
    int audio_cfg;
    int keyframes;
} counts;

static int drain_and_write(zm_reader *r, zm_writer *w, counts *cnt)
{
    zm_packet pkt;

    while (zm_reader_read(r, &pkt) == 1) {
        if (pkt.kind == ZM_KIND_VIDEO) {
            cnt->video++;
        } else if (pkt.kind == ZM_KIND_AUDIO) {
            cnt->audio++;
        }
        if (pkt.flags & ZM_PKT_CONFIG) {
            if (pkt.kind == ZM_KIND_VIDEO) {
                cnt->video_cfg++;
            } else if (pkt.kind == ZM_KIND_AUDIO) {
                cnt->audio_cfg++;
            }
        }
        if (pkt.flags & ZM_PKT_KEY) {
            cnt->keyframes++;
        }
        if (w) {
            CHECK(zm_writer_write(w, &pkt) == ZM_OK);
        }
        zm_packet_clear(&pkt);
    }
    return 0;
}

static int feed_tags(zm_reader *r, void *flv, zm_writer *w, counts *cnt)
{
    uint8_t *buf = (uint8_t *)malloc(1 << 20);
    int type;
    uint32_t ts;
    size_t taglen;
    int rc;

    if (!buf) {
        return -1;
    }
    while ((rc = flv_reader_read(flv, &type, &ts, &taglen, buf, 1 << 20)) == 1) {
        if (taglen == 0) {
            continue;
        }
        if (zm_flv_reader_input_tag(r, type, buf, taglen, ts) != ZM_OK) {
            free(buf);
            return -1;
        }
        drain_and_write(r, w, cnt);
    }
    free(buf);
    return rc == 0 ? 0 : -1;
}

typedef struct file_ctx {
    FILE *fp;
} file_ctx;

static int file_read(void *param, void *buf, size_t len)
{
    file_ctx *c = (file_ctx *)param;
    size_t n = fread(buf, 1, len, c->fp);
    if (n > 0) {
        return (int)n;
    }
    return ferror(c->fp) ? -1 : 0;
}

static int file_seek(void *param, int64_t off, int whence)
{
    file_ctx *c = (file_ctx *)param;
    return fseek(c->fp, (long)off, whence) == 0 ? 0 : -1;
}

static int64_t file_tell(void *param)
{
    file_ctx *c = (file_ctx *)param;
    return (int64_t)ftell(c->fp);
}

static void test_file_mode(const char *path)
{
    file_ctx fc;
    zm_io io;
    zm_reader_cfg rcfg;
    zm_reader *r;
    const zm_track *tracks;
    size_t track_count = 0;
    counts cnt;
    zm_packet pkt;
    int64_t ms;
    int got_key = 0;
    int i;

    memset(&fc, 0, sizeof(fc));
    fc.fp = fopen(path, "rb");
    CHECK(fc.fp != NULL);
    if (!fc.fp) {
        return;
    }
    memset(&io, 0, sizeof(io));
    io.ctx = &fc;
    io.read = file_read;
    io.seek = file_seek;
    io.tell = file_tell;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = ZM_FMT_FLV;
    rcfg.mode = ZM_READ_ES;
    r = zm_flv_reader_open_file(&io, &rcfg);
    CHECK(r != NULL);
    if (!r) {
        fclose(fc.fp);
        return;
    }
    tracks = zm_reader_tracks(r, &track_count);
    CHECK(tracks != NULL && track_count == 2);
    CHECK(tracks && tracks[0].codec != ZM_CODEC_INVALID);
    CHECK(tracks && tracks[0].config.len > 0); /* 文件模式 config 走 track */

    memset(&cnt, 0, sizeof(cnt));
    while (zm_reader_read(r, &pkt) == 1) {
        if (pkt.flags & ZM_PKT_CONFIG) {
            if (pkt.kind == ZM_KIND_VIDEO) {
                cnt.video_cfg++;
            } else if (pkt.kind == ZM_KIND_AUDIO) {
                cnt.audio_cfg++;
            }
        }
        if (pkt.kind == ZM_KIND_VIDEO) {
            cnt.video++;
        } else if (pkt.kind == ZM_KIND_AUDIO) {
            cnt.audio++;
        }
        if (pkt.flags & ZM_PKT_KEY) {
            cnt.keyframes++;
        }
        zm_packet_clear(&pkt);
    }
    CHECK(cnt.video > 0 && cnt.audio > 0);
    CHECK(cnt.keyframes > 0);
    CHECK(cnt.video_cfg == 0 && cnt.audio_cfg == 0); /* 文件模式不产出 config 包 */

    ms = 1000;
    CHECK(zm_reader_seek(r, &ms) == ZM_OK);
    (void)ms;
    for (i = 0; i < 64; ++i) {
        if (zm_reader_read(r, &pkt) != 1) {
            break;
        }
        if (pkt.kind == ZM_KIND_VIDEO && (pkt.flags & ZM_PKT_KEY)) {
            got_key = 1;
        }
        zm_packet_clear(&pkt);
        if (got_key) {
            break;
        }
    }
    CHECK(got_key);

    zm_reader_close(r);
    fclose(fc.fp);
}

int main(void)
{
    const char *path = getenv("ZCS_FIXTURE");
    zm_reader_cfg rcfg;
    zm_writer_cfg wcfg;
    zm_reader *r1;
    zm_reader *r2;
    zm_reader *r3;
    void *fr;
    const zm_track *tracks;
    size_t track_count;
    counts in;
    counts out;
    capture cap;
    zm_sink sink;
    zm_writer *w;
    mem_io mem;
    int i;

    if (!path || !*path) {
        path = ZCS_FIXTURE_DIR "/h264_aac.flv";
    }
    fr = flv_reader_create(path);
    if (!fr) {
        printf("test_zm_flv_file: skip (no %s)\n", path);
        return 0;
    }

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = ZM_FMT_FLV;
    rcfg.mode = ZM_READ_ES;
    rcfg.input_tag_body = 1;
    r1 = zm_flv_reader_open_push(&rcfg);
    CHECK(r1 != NULL);
    memset(&in, 0, sizeof(in));
    if (!r1 || feed_tags(r1, fr, NULL, &in) != 0) {
        CHECK(0);
        flv_reader_destroy(fr);
        if (r1) {
            zm_reader_close(r1);
        }
        return 1;
    }
    tracks = zm_reader_tracks(r1, &track_count);
    CHECK(tracks != NULL);
    CHECK(track_count == 2);
    CHECK(in.video > 0 && in.audio > 0);
    CHECK(in.video_cfg > 0 && in.audio_cfg > 0);
    CHECK(in.keyframes > 0);
    flv_reader_destroy(fr);

    cap.cap = 2 << 20;
    cap.len = 0;
    cap.data = (uint8_t *)malloc(cap.cap);
    CHECK(cap.data != NULL);
    if (!cap.data) {
        zm_reader_close(r1);
        return 1;
    }
    memset(&wcfg, 0, sizeof(wcfg));
    sink = make_sink(&cap);
    wcfg.format = ZM_FMT_FLV;
    wcfg.mode = ZM_WRITE_ES;
    wcfg.tracks = tracks;
    wcfg.track_count = track_count;
    wcfg.tag_body = 0;
    w = zm_flv_writer_open(&sink, &wcfg);
    CHECK(w != NULL);
    zm_reader_close(r1);
    if (!w) {
        free(cap.data);
        return 1;
    }

    fr = flv_reader_create(path);
    CHECK(fr != NULL);
    r2 = zm_flv_reader_open_push(&rcfg);
    CHECK(r2 != NULL);
    memset(&out, 0, sizeof(out));
    if (fr && r2) {
        CHECK(feed_tags(r2, fr, w, &out) == 0);
    }
    CHECK(out.video == in.video);
    CHECK(out.audio == in.audio);
    if (fr) {
        flv_reader_destroy(fr);
    }
    if (r2) {
        zm_reader_close(r2);
    }
    zm_writer_close(w);
    CHECK(cap.len > 13);

    /* Read the generated FLV back and compare media packet counts. */
    memset(&mem, 0, sizeof(mem));
    mem.data = cap.data;
    mem.len = cap.len;
    fr = flv_reader_create2(mem_read, &mem);
    CHECK(fr != NULL);
    r3 = zm_flv_reader_open_push(&rcfg);
    CHECK(r3 != NULL);
    memset(&out, 0, sizeof(out));
    if (fr && r3) {
        CHECK(feed_tags(r3, fr, NULL, &out) == 0);
    }
    CHECK(out.video == in.video);
    CHECK(out.audio == in.audio);
    CHECK(out.video_cfg > 0);
    CHECK(out.audio_cfg > 0);
    if (fr) {
        flv_reader_destroy(fr);
    }
    if (r3) {
        zm_reader_close(r3);
    }
    free(cap.data);

    test_file_mode(path);

    if (g_failures) {
        fprintf(stderr, "test_zm_flv_file: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_flv_file ok (video=%d audio=%d keyframes=%d)\n", in.video, in.audio,
           in.keyframes);
    return 0;
}
