/**
 * @file test_zm_mkv.c
 * @brief Matroska reader facade against a generated fixture.
 *
 * Skips cleanly when tests/fixtures/h264_aac.mkv is absent. Generate it with
 * tests/fixtures/generate.sh.
 */
#include "zm/format/mkv.h"

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

int main(void)
{
    const char *path = getenv("ZMS_FIXTURE");
    if (!path || !*path) {
        path = ZMS_FIXTURE_DIR "/h264_aac.mkv";
    }
    file_io f;
    zm_io io;
    zm_reader *r;
    const zm_track *tracks;
    size_t track_count;
    size_t i;
    int have_video = 0;
    int have_audio = 0;
    int video_packets = 0;
    int audio_packets = 0;
    int keyframes = 0;
    int packets = 0;
    zm_packet pkt;

    f.fp = fopen(path, "rb");
    if (!f.fp) {
        printf("test_zm_mkv: skip (no %s)\n", path);
        return 0;
    }
    memset(&io, 0, sizeof(io));
    io.ctx = &f;
    io.read = file_read;
    io.seek = file_seek;
    io.tell = file_tell;

    r = zm_mkv_reader_open(&io, NULL);
    CHECK(r != NULL);
    if (!r) {
        fclose(f.fp);
        return 1;
    }

    tracks = zm_reader_tracks(r, &track_count);
    CHECK(tracks != NULL);
    CHECK(track_count > 0);
    for (i = 0; tracks && i < track_count; ++i) {
        if (tracks[i].kind == ZM_KIND_VIDEO) {
            int is_video = 0;
            switch (tracks[i].codec) {
            case ZM_CODEC_H264:
                is_video = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_AVCC);
                break;
            case ZM_CODEC_H265:
                is_video = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_HVCC);
                break;
            case ZM_CODEC_H266:
                is_video = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_VVCC);
                break;
            case ZM_CODEC_AV1:
                is_video = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_AV1C);
                break;
            default:
                break;
            }
            if (is_video) {
                have_video = 1;
                CHECK(tracks[i].width > 0 && tracks[i].height > 0);
            }
        } else if (tracks[i].kind == ZM_KIND_AUDIO) {
            int is_audio = 0;
            if (tracks[i].codec == ZM_CODEC_AAC) {
                is_audio = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_ASC);
            } else if (tracks[i].codec == ZM_CODEC_OPUS) {
                is_audio = 1;
                CHECK(tracks[i].config.fmt == ZM_CONFIG_OPUS_HEAD);
            }
            if (is_audio) {
                have_audio = 1;
                CHECK(tracks[i].sample_rate > 0);
            }
        }
    }
    CHECK(have_video);
    CHECK(have_audio);

    while (packets < 400) {
        int rc;
        zm_packet_init(&pkt);
        rc = zm_reader_read(r, &pkt);
        if (rc <= 0) {
            break;
        }
        ++packets;
        CHECK(pkt.mem.len > 0);
        CHECK(pkt.mem.data != NULL);
        if (pkt.kind == ZM_KIND_VIDEO) {
            ++video_packets;
            CHECK(pkt.es_format == ZM_ES_ANNEXB || pkt.es_format == ZM_ES_RAW);
        } else if (pkt.kind == ZM_KIND_AUDIO) {
            ++audio_packets;
            CHECK(pkt.es_format == ZM_ES_RAW);
        }
        if (pkt.flags & ZM_PKT_KEY) {
            ++keyframes;
        }
        zm_packet_clear(&pkt);
    }
    CHECK(video_packets > 0);
    CHECK(audio_packets > 0);
    CHECK(keyframes > 0);

    zm_reader_close(r);
    fclose(f.fp);

    if (g_failures) {
        fprintf(stderr, "test_zm_mkv: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_mkv ok (%d packets)\n", packets);
    return 0;
}
