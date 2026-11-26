/**
 * @file test_zm_mov_write.c
 * @brief MP4 writer -> MP4 reader round trip for an AAC-only track.
 */
#include "zm/format/mov.h"

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

typedef struct file_sink {
    FILE *fp;
} file_sink;

static int sink_write(void *ctx, const void *data, size_t len)
{
    file_sink *s = (file_sink *)ctx;
    return fwrite(data, 1, len, s->fp) == len ? 0 : -1;
}

static int sink_seek(void *ctx, int64_t off)
{
    file_sink *s = (file_sink *)ctx;
    return fseek(s->fp, (long)off, off >= 0 ? SEEK_SET : SEEK_END) == 0 ? 0 : -1;
}

static int sink_tell(void *ctx, int64_t *off)
{
    file_sink *s = (file_sink *)ctx;
    long pos = ftell(s->fp);
    if (pos < 0 || !off) {
        return -1;
    }
    *off = (int64_t)pos;
    return 0;
}

static int file_read(void *ctx, void *buf, size_t len)
{
    file_sink *s = (file_sink *)ctx;
    return (int)fread(buf, 1, len, s->fp);
}

static int file_seek(void *ctx, int64_t off, int whence)
{
    file_sink *s = (file_sink *)ctx;
    return fseek(s->fp, (long)off, whence) == 0 ? 0 : -1;
}

static int64_t file_tell(void *ctx)
{
    file_sink *s = (file_sink *)ctx;
    return (int64_t)ftell(s->fp);
}

int main(void)
{
    static const uint8_t k_asc[] = {0x12, 0x10};
    static const uint8_t k_frames[3][8] = {
        {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
        {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38},
        {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48},
    };
    const char *path = "test_zm_mov_write.tmp";
    zm_track track;
    zm_writer_cfg wcfg;
    zm_writer *w;
    file_sink s;
    zm_sink sink;
    zm_reader *r;
    zm_io io;
    const zm_track *tracks;
    size_t track_count;
    size_t i;
    int got = 0;
    zm_packet pkt;

    s.fp = fopen(path, "w+b");
    CHECK(s.fp != NULL);
    if (!s.fp) {
        return 1;
    }
    memset(&sink, 0, sizeof(sink));
    sink.ctx = &s;
    sink.write = sink_write;
    sink.seek = sink_seek;
    sink.tell = sink_tell;

    zm_track_init(&track);
    track.track_id = 1;
    track.codec = ZM_CODEC_AAC;
    track.kind = ZM_KIND_AUDIO;
    track.clock_hz = 44100;
    track.sample_rate = 44100;
    track.channels = 2;
    track.bits_per_sample = 16;
    track.timebase.num = 1;
    track.timebase.den = 1000;
    zm_track_set_config(&track, ZM_CONFIG_ASC, k_asc, sizeof(k_asc));

    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.format = ZM_FMT_MOV;
    wcfg.mode = ZM_WRITE_ES;
    wcfg.tracks = &track;
    wcfg.track_count = 1;
    w = zm_mov_writer_open(&sink, &wcfg);
    CHECK(w != NULL);
    if (!w) {
        fclose(s.fp);
        zm_track_clear(&track);
        return 1;
    }

    for (i = 0; i < 3; ++i) {
        zm_packet_init(&pkt);
        pkt.mem = zm_mem_borrow(k_frames[i], sizeof(k_frames[i]));
        pkt.codec = ZM_CODEC_AAC;
        pkt.kind = ZM_KIND_AUDIO;
        pkt.track_id = 1;
        pkt.dts_ms = (int64_t)i * 100;
        pkt.pts_ms = pkt.dts_ms;
        pkt.es_format = ZM_ES_RAW;
        CHECK(zm_writer_write(w, &pkt) == ZM_OK);
    }
    CHECK(zm_writer_finish(w) == ZM_OK);
    zm_writer_close(w);
    fflush(s.fp);
    fclose(s.fp);
    zm_track_clear(&track);

    s.fp = fopen(path, "rb");
    CHECK(s.fp != NULL);
    if (!s.fp) {
        remove(path);
        return 1;
    }
    memset(&io, 0, sizeof(io));
    io.ctx = &s;
    io.read = file_read;
    io.seek = file_seek;
    io.tell = file_tell;
    r = zm_mov_reader_open(&io, NULL);
    CHECK(r != NULL);
    if (!r) {
        fclose(s.fp);
        remove(path);
        return 1;
    }

    tracks = zm_reader_tracks(r, &track_count);
    CHECK(tracks != NULL);
    CHECK(track_count == 1);
    if (tracks && track_count == 1) {
        CHECK(tracks[0].codec == ZM_CODEC_AAC);
        CHECK(tracks[0].kind == ZM_KIND_AUDIO);
        CHECK(tracks[0].config.fmt == ZM_CONFIG_ASC);
        CHECK(tracks[0].config.len == sizeof(k_asc));
        CHECK(tracks[0].sample_rate == 44100);
    }

    while (zm_reader_read(r, &pkt) == 1) {
        CHECK(pkt.codec == ZM_CODEC_AAC);
        CHECK(pkt.mem.len == sizeof(k_frames[0]));
        if (got < 3) {
            CHECK(memcmp(pkt.mem.data, k_frames[got], sizeof(k_frames[0])) == 0);
            CHECK(pkt.dts_ms >= (int64_t)got * 100 - 2 &&
                  pkt.dts_ms <= (int64_t)got * 100 + 2);
        }
        zm_packet_clear(&pkt);
        ++got;
    }
    CHECK(got == 3);

    zm_reader_close(r);
    fclose(s.fp);
    remove(path);

    if (g_failures) {
        fprintf(stderr, "test_zm_mov_write: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_mov_write ok (%d frames)\n", got);
    return 0;
}
