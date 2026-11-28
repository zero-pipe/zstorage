/**
 * @file test_vod_file_demux.c
 * @brief VOD 文件 demux 回归：MP4/MOV 与 MKV 走 zm_*_reader 后的行为契约。
 *
 * 校验：config 包、媒体包（Annex-B/关键帧）、duration、seek、EOF，
 * 以及 MP4 sample 表遍历与文件型 zm_io。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/media/codec/codec_id.h"

#include "bridge/zm_file_io.h"
#include "zm/core/io.h"

#include <stdio.h>
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

typedef struct {
    int video_cfg;
    int audio_cfg;
    int video_samples;
    int audio_samples;
    int key_seen;
    int annexb_seen;
} collector;

static int fixture_present(const char *name)
{
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", ZCS_FIXTURE_DIR, name);
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static int is_annexb(const uint8_t *p, size_t len)
{
    if (!p || len < 3) {
        return 0;
    }
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 1;
    }
    return len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

static void on_packet(const zcs_container_packet *p, void *user)
{
    collector *c = (collector *)user;

    if (!p) {
        return;
    }
    if (p->config) {
        if (p->codec == ZCS_CODEC_H264 || p->codec == ZCS_CODEC_H265) {
            c->video_cfg++;
        } else if (p->codec == ZCS_CODEC_AAC) {
            c->audio_cfg++;
        }
        return;
    }
    if (p->codec == ZCS_CODEC_H264 || p->codec == ZCS_CODEC_H265) {
        c->video_samples++;
        if (p->key) {
            c->key_seen++;
        }
        if (is_annexb(p->data, p->len)) {
            c->annexb_seen++;
        }
    } else if (p->codec == ZCS_CODEC_AAC) {
        c->audio_samples++;
    }
}

static int count_sample(const zcs_mp4_sample_info *s, void *user)
{
    int *n = (int *)user;

    if (s) {
        (*n)++;
    }
    return 0;
}

static void test_zm_file_io(void)
{
    char path[1024];
    zm_io io;
    uint8_t buf[16];
    int n;

    snprintf(path, sizeof(path), "%s/h264_aac.mp4", ZCS_FIXTURE_DIR);
    if (!fixture_present("h264_aac.mp4")) {
        printf("test_vod_file_demux: skip file io (fixture missing)\n");
        return;
    }
    memset(&io, 0, sizeof(io));
    CHECK(zcs_zm_file_io_open(path, &io) == 0);
    CHECK(io.read != NULL && io.seek != NULL && io.tell != NULL);

    n = io.read(io.ctx, buf, sizeof(buf));
    CHECK(n == (int)sizeof(buf));
    CHECK(memcmp(buf + 4, "ftyp", 4) == 0); /* MP4 box type at offset 4 */
    CHECK(io.tell(io.ctx) == (int64_t)sizeof(buf));
    CHECK(io.seek(io.ctx, 0, SEEK_SET) == 0);
    CHECK(io.tell(io.ctx) == 0);

    zcs_zm_file_io_close(&io);
    CHECK(io.ctx == NULL && io.read == NULL);
}

static void test_container(zcs_container_id id, const char *file)
{
    char path[1024];
    const zcs_container_demuxer_ops *ops;
    zcs_container_demux_opts cfg;
    collector col;
    zcs_codec_id video = ZCS_CODEC_INVALID;
    zcs_codec_id audio = ZCS_CODEC_INVALID;
    void *ctx;
    uint64_t dur;
    int64_t ms;
    int v0;
    int i;

    snprintf(path, sizeof(path), "%s/%s", ZCS_FIXTURE_DIR, file);
    if (!fixture_present(file)) {
        printf("test_vod_file_demux: skip %s (fixture missing)\n", file);
        return;
    }
    ops = zcs_container_demuxer_find(id);
    CHECK(ops != NULL && ops->create && ops->open_file && ops->pump);
    if (!ops || !ops->create) {
        return;
    }

    memset(&col, 0, sizeof(col));
    memset(&cfg, 0, sizeof(cfg));
    cfg.id = id;
    cfg.on_packet = on_packet;
    cfg.user = &col;

    ctx = ops->create(&cfg);
    CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }
    CHECK(ops->open_file(ctx, path) == ZN_OK);

    if (id == ZCS_CONTAINER_MP4) {
        zcs_container_demux_mp4_codecs(ctx, &video, &audio);
    } else if (id == ZCS_CONTAINER_MKV) {
        zcs_container_demux_mkv_codecs(ctx, &video, &audio);
    } else {
        zcs_container_demux_flv_file_codecs(ctx, &video, &audio);
    }
    CHECK(video == ZCS_CODEC_H264 || video == ZCS_CODEC_H265);
    CHECK(audio == ZCS_CODEC_AAC);
    CHECK(col.video_cfg >= 1);
    CHECK(col.audio_cfg >= 1);

    dur = ops->duration_ms ? ops->duration_ms(ctx) : 0;
    CHECK(dur > 0);

    for (i = 0; i < 200000; ++i) {
        int r = ops->pump(ctx);
        if (r <= 0) {
            break;
        }
    }
    CHECK(col.video_samples > 0);
    CHECK(col.audio_samples > 0);
    CHECK(col.key_seen > 0);
    CHECK(col.annexb_seen > 0);

    if (ops->seek) {
        ms = 2000;
        CHECK(ops->seek(ctx, &ms) == ZN_OK);
        /* 最近关键帧语义：落点可能在目标附近（前或后）。 */
        CHECK(ms >= 0 && ms <= 60000);
        v0 = col.video_samples;
        for (i = 0; i < 500; ++i) {
            if (ops->pump(ctx) <= 0) {
                break;
            }
        }
        CHECK(col.video_samples > v0);
    }

    if (id == ZCS_CONTAINER_MP4) {
        int samples = 0;
        int rc = zcs_container_mp4_for_each_sample(ctx, count_sample, &samples);
        CHECK(rc > 0);
        CHECK(samples > 0);
    }

    if (ops->destroy) {
        ops->destroy(ctx);
    }
}

int main(void)
{
    zcs_container_register_all();

    test_zm_file_io();

    test_container(ZCS_CONTAINER_MP4, "h264_aac.mp4");
    test_container(ZCS_CONTAINER_MP4, "h265_aac.mp4");
    test_container(ZCS_CONTAINER_MKV, "h264_aac.mkv");
    test_container(ZCS_CONTAINER_MKV, "h265_aac.mkv");
    test_container(ZCS_CONTAINER_FLV_FILE, "h264_aac.flv");

    if (g_failures) {
        fprintf(stderr, "test_vod_file_demux: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_vod_file_demux: ok\n");
    return 0;
}
