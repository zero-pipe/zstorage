/**
 * @file reader.c
 * @brief VOD 统一 reader（预填与 poller 绑定）。
 */
#include "zcs/vod/io/reader.h"
#include "zcs/vod/io/file_reader.h"
#include "zcs/media/container/flv/flv_index.h"
#include <stdlib.h>
#include <string.h>

struct zcs_vod_reader {
    zcs_vod_file_reader *impl;
};

zcs_vod_reader *zcs_vod_reader_open(const zcs_vod_reader_opts *opts)
{
    zcs_vod_file_reader_opts mp4_opts;
    zcs_vod_reader *r;

    if (!opts) {
        return NULL;
    }
    r = (zcs_vod_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    memset(&mp4_opts, 0, sizeof(mp4_opts));
    mp4_opts.file_path = opts->file_path;
    mp4_opts.app = opts->app;
    mp4_opts.stream = opts->stream;
    mp4_opts.fifo = opts->fifo;
    mp4_opts.source = opts->source;
    mp4_opts.speed = opts->speed;
    mp4_opts.loop = opts->loop;
    mp4_opts.fifo_high_water = opts->fifo_high_water;
    mp4_opts.owner_ctx = r;
    r->impl = zcs_vod_file_reader_open(&mp4_opts);
    if (!r->impl) {
        free(r);
        return NULL;
    }
    return r;
}

void zcs_vod_reader_close(zcs_vod_reader *r)
{
    if (!r) {
        return;
    }
    zcs_vod_file_reader_close(r->impl);
    free(r);
}

void zcs_vod_reader_bind_poller(zcs_vod_reader *r, zn_poller *poller)
{
    if (r) {
        zcs_vod_file_reader_bind_poller(r->impl, poller);
    }
}

void zcs_vod_reader_bind_poller_lite(zcs_vod_reader *r, zn_poller *poller)
{
    if (r) {
        zcs_vod_file_reader_bind_poller_lite(r->impl, poller);
    }
}

void zcs_vod_reader_prefill(zcs_vod_reader *r)
{
    if (r) {
        zcs_vod_file_reader_prefill(r->impl);
    }
}

void zcs_vod_reader_prepare_play(zcs_vod_reader *r)
{
    if (r) {
        zcs_vod_file_reader_prepare_play(r->impl);
    }
}

uint64_t zcs_vod_reader_seek_ms(zcs_vod_reader *r, uint64_t ms)
{
    return r ? zcs_vod_file_reader_seek_ms(r->impl, ms) : 0;
}

zcs_media_source *zcs_vod_reader_source(zcs_vod_reader *r)
{
    return r ? zcs_vod_file_reader_source(r->impl) : NULL;
}

const char *zcs_vod_reader_file_path(const zcs_vod_reader *r)
{
    return r ? zcs_vod_file_reader_file_path(r->impl) : NULL;
}

uint64_t zcs_vod_reader_duration_ms(const zcs_vod_reader *r)
{
    return r ? zcs_vod_file_reader_duration_ms(r->impl) : 0;
}

const zcs_vod_flv_index *zcs_vod_reader_flv_index(zcs_vod_reader *r)
{
    return r ? zcs_vod_file_reader_flv_index(r->impl) : NULL;
}

void zcs_vod_reader_set_pump_hold(zcs_vod_reader *r, int hold)
{
    if (r) {
        zcs_vod_file_reader_set_pump_hold(r->impl, hold);
    }
}

int zcs_vod_reader_pump(zcs_vod_reader *r)
{
    return r ? zcs_vod_file_reader_pump(r->impl) : 0;
}
