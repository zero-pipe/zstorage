/**
 * @file play_lane.c
 * @brief VOD play lane：reader、缓冲、seek 与追帧。
 */
#include "zcs/vod/play/play_lane.h"
#include "zcs/vod/io/source.h"
#include "zcs/core/stream/hub.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

#define VOD_LANE_BUFFER_CAP 256u
#define VOD_LANE_FIFO_HW 128u

struct zcs_vod_play_lane {
    zcs_vod_reader *reader;
    zcs_vod_buffer *fifo;
    zcs_vod_buffer_reader *fifo_rd;
    zcs_vod_reader *pub_hold;
    zcs_media_source *source;
};

static void vod_lane_pub_hold(zcs_media_source *src, zcs_vod_reader *pub)
{
    if (!src || !pub) {
        return;
    }
    zcs_media_source_registry_lock();
    if (++src->vod_play_lane_ref == 1) {
        zcs_vod_reader_set_pump_hold(pub, 1);
    }
    zcs_media_source_registry_unlock();
}

static void vod_lane_pub_release(zcs_media_source *src, zcs_vod_reader *pub)
{
    if (!src || !pub) {
        return;
    }
    zcs_media_source_registry_lock();
    if (src->vod_play_lane_ref > 0 && --src->vod_play_lane_ref == 0) {
        zcs_vod_reader_set_pump_hold(pub, 0);
    }
    zcs_media_source_registry_unlock();
}

static void vod_lane_align_reader(zcs_vod_play_lane *lane)
{
    if (!lane || !lane->fifo_rd) {
        return;
    }
    zcs_vod_buffer_reader_seek_beginning(lane->fifo_rd);
    zcs_vod_buffer_reader_seek_video_key(lane->fifo_rd);
}

zcs_vod_play_lane *zcs_vod_play_lane_open(const zcs_media_source *src, zn_poller *poller)
{
    zcs_vod_reader_opts opts;
    zcs_vod_play_lane *lane;
    char path[512];

    if (!src) {
        return NULL;
    }
    /* loadMP4File 等显式路径优先；经典 www/{app}/{stream} 布局作回退 */
    if (!zcs_vod_source_file_path(src, path, sizeof(path)) &&
        !zcs_vod_resolve_file_path(src->app, src->stream, path, sizeof(path))) {
        zn_warn("vod lane: file not found %s/%s", src->app, src->stream);
        return NULL;
    }

    lane = (zcs_vod_play_lane *)calloc(1, sizeof(*lane));
    if (!lane) {
        return NULL;
    }

    lane->fifo = zcs_vod_buffer_create(VOD_LANE_BUFFER_CAP);
    if (!lane->fifo) {
        goto fail;
    }

    memset(&opts, 0, sizeof(opts));
    opts.file_path = path;
    opts.app = src->app;
    opts.stream = src->stream;
    opts.fifo = lane->fifo;
    opts.source = (zcs_media_source *)src;
    opts.speed = 1.0;
    opts.fifo_high_water = VOD_LANE_FIFO_HW;

    lane->reader = zcs_vod_reader_open(&opts);
    if (!lane->reader) {
        goto fail;
    }

    if (poller) {
        zcs_vod_reader_bind_poller_lite(lane->reader, poller);
    }
    lane->fifo_rd = zcs_vod_buffer_reader_attach(lane->fifo, 1);
    if (!lane->fifo_rd) {
        goto fail;
    }

    if (src->publisher_ctx && src->publisher_ctx != lane->reader) {
        lane->pub_hold = (zcs_vod_reader *)src->publisher_ctx;
        lane->source = (zcs_media_source *)src;
        vod_lane_pub_hold(lane->source, lane->pub_hold);
    }

    zn_info("vod lane: open %s -> %s/%s", path, src->app, src->stream);
    return lane;

fail:
    zcs_vod_play_lane_close(lane);
    return NULL;
}

void zcs_vod_play_lane_close(zcs_vod_play_lane *lane)
{
    if (!lane) {
        return;
    }
    if (lane->pub_hold) {
        vod_lane_pub_release(lane->source, lane->pub_hold);
        lane->pub_hold = NULL;
        lane->source = NULL;
    }
    zcs_vod_buffer_reader_detach(lane->fifo_rd);
    lane->fifo_rd = NULL;
    zcs_vod_reader_close(lane->reader);
    lane->reader = NULL;
    zcs_vod_buffer_destroy(lane->fifo);
    lane->fifo = NULL;
    free(lane);
}

zcs_vod_buffer_reader *zcs_vod_play_lane_buffer_reader(zcs_vod_play_lane *lane)
{
    return lane ? lane->fifo_rd : NULL;
}

zcs_vod_reader *zcs_vod_play_lane_reader(zcs_vod_play_lane *lane)
{
    return lane ? lane->reader : NULL;
}

uint64_t zcs_vod_play_lane_seek_ms(zcs_vod_play_lane *lane, uint64_t ms)
{
    if (!lane || !lane->reader) {
        return 0;
    }
    ms = zcs_vod_reader_seek_ms(lane->reader, ms);
    vod_lane_align_reader(lane);
    return ms;
}

uint64_t zcs_vod_play_lane_prepare(zcs_vod_play_lane *lane, uint64_t start_ms)
{
    return zcs_vod_play_lane_seek_ms(lane, start_ms);
}

void zcs_vod_play_lane_prefill(zcs_vod_play_lane *lane)
{
    if (lane && lane->reader) {
        zcs_vod_reader_prefill(lane->reader);
    }
}

void zcs_vod_play_lane_align_reader(zcs_vod_play_lane *lane)
{
    vod_lane_align_reader(lane);
}

void zcs_vod_play_lane_set_pump_hold(zcs_vod_play_lane *lane, int hold)
{
    if (lane && lane->reader) {
        zcs_vod_reader_set_pump_hold(lane->reader, hold);
    }
}

void zcs_vod_play_lane_demux_fill(zcs_vod_play_lane *lane, int max_pumps)
{
    int i;
    size_t target_lag = 48;

    if (!lane || !lane->reader || !lane->fifo_rd || max_pumps <= 0) {
        return;
    }
    for (i = 0; i < max_pumps; ++i) {
        if (zcs_vod_buffer_reader_lag(lane->fifo_rd) >= target_lag) {
            break;
        }
        if (zcs_vod_reader_pump(lane->reader) <= 0) {
            break;
        }
    }
}

const uint8_t *zcs_vod_play_lane_video_config(const zcs_vod_play_lane *lane, size_t *len)
{
    return lane && lane->fifo ? zcs_vod_buffer_video_config(lane->fifo, len) : NULL;
}

const uint8_t *zcs_vod_play_lane_audio_config(const zcs_vod_play_lane *lane, size_t *len)
{
    return lane && lane->fifo ? zcs_vod_buffer_audio_config(lane->fifo, len) : NULL;
}
