/**
 * @file gb_playback.c
 * @brief GB28181 录像查询（RecordInfo）与回放（VOD → PS/RTP）。
 */
#include "device/gb28181/gb28181_internal.h"

#include "zms/core/stream/buffer.h"
#include "zms/vod/io/reader.h"
#include "zms/util/buf_pool.h"
#include "zn/platform.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gb_playback {
    zms_vod_reader *reader;
    zms_vod_buffer *fifo;
    zms_vod_buffer_reader *rd;
    zms_media_source src;
    zms_media_source *source;
    uint8_t *es_buf;
    size_t es_cap;
    uint64_t last_data_ms;
    int started;
};

gb_record_list *gb_record_query(const char *channel_id, const char *start_time,
                                const char *end_time)
{
    return zms_device_record_query(channel_id, start_time, end_time);
}

/* ── 回放 ───────────────────────────────────────────────────────────────── */

gb_playback *gb_playback_open(const char *channel_id, const char *start_time,
                              const char *end_time, zn_poller *poller)
{
    gb_playback *p;
    zms_vod_reader_opts opts;
    zms_device_record_entry rec;
    zms_device_channel_info ch;
    char file_path[768];
    const char *root = zms_device_core_record_root();
    int64_t offset_ms = 0;

    if (!channel_id || !root[0]) {
        return NULL;
    }
    if (zms_device_core_get(channel_id, &ch) != 0) {
        return NULL;
    }
    if (zms_device_record_pick(channel_id, start_time, end_time, &rec, &offset_ms) != 0) {
        return NULL;
    }
    snprintf(file_path, sizeof(file_path), "%s/%s", root, rec.file_path);

    p = (gb_playback *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->fifo = zms_vod_buffer_create(512);
    if (!p->fifo) {
        free(p);
        return NULL;
    }
    memset(&p->src, 0, sizeof(p->src));
    memset(&opts, 0, sizeof(opts));
    opts.file_path = file_path;
    opts.app = "gb28181";
    opts.stream = ch.stream;
    opts.fifo = p->fifo;
    opts.source = &p->src;
    opts.speed = 1.0;
    opts.loop = 0;
    opts.fifo_high_water = 256;
    p->reader = zms_vod_reader_open(&opts);
    if (!p->reader) {
        zms_vod_buffer_destroy(p->fifo);
        free(p);
        return NULL;
    }
    p->source = &p->src;
    zms_vod_reader_prepare_play(p->reader);
    if (offset_ms > 0) {
        (void)zms_vod_reader_seek_ms(p->reader, (uint64_t)offset_ms);
    }
    if (poller) {
        zms_vod_reader_bind_poller(p->reader, poller);
    }
    p->rd = zms_vod_buffer_reader_attach(p->fifo, 1);
    if (!p->rd) {
        zms_vod_reader_close(p->reader);
        zms_vod_buffer_destroy(p->fifo);
        free(p);
        return NULL;
    }
    zms_vod_buffer_reader_seek_video_key(p->rd);
    p->last_data_ms = zn_monotonic_ms();
    p->started = 1;
    zn_info("gb28181: playback open channel=%s file=%s offset=%lldms", channel_id, file_path,
             (long long)offset_ms);
    return p;
}

zms_media_source *gb_playback_source(gb_playback *p)
{
    return p ? p->source : NULL;
}

const uint8_t *gb_playback_video_config(gb_playback *p, size_t *len)
{
    if (!p || !p->fifo) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    return zms_vod_buffer_video_config(p->fifo, len);
}

const uint8_t *gb_playback_audio_config(gb_playback *p, size_t *len)
{
    if (!p || !p->fifo) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    return zms_vod_buffer_audio_config(p->fifo, len);
}

int gb_playback_next(gb_playback *p, zms_gop_slot *slot, int timeout_ms)
{
    uint64_t deadline;

    if (!p || !p->rd || !slot) {
        return -1;
    }
    deadline = zn_monotonic_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 50);
    for (;;) {
        if (zms_vod_buffer_reader_peek_muxed_es(p->rd, slot, &p->es_buf, &p->es_cap, NULL)) {
            zms_vod_buffer_reader_advance(p->rd);
            p->last_data_ms = zn_monotonic_ms();
            return 1;
        }
        if (zn_monotonic_ms() >= deadline) {
            if (zn_monotonic_ms() - p->last_data_ms > 3000u) {
                return -1;
            }
            return 0;
        }
        zn_sleep_ms(5);
    }
}

void gb_playback_close(gb_playback *p)
{
    if (!p) {
        return;
    }
    if (p->rd) {
        zms_vod_buffer_reader_detach(p->rd);
        p->rd = NULL;
    }
    if (p->reader) {
        zms_vod_reader_close(p->reader);
        p->reader = NULL;
    }
    if (p->fifo) {
        zms_vod_buffer_destroy(p->fifo);
        p->fifo = NULL;
    }
    zms_buf_pool_slot_clear(&p->es_buf, &p->es_cap);
    free(p);
}
