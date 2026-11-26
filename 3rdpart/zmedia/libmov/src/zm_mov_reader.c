#include "zm/format/mov.h"

#include "zm/core/codec.h"

#include "mov-format.h"
#include "mov-reader.h"

#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZM_MOV_MAX_TRACKS 8

typedef struct zm_mov_track_state {
    uint32_t track_id;
    int codec_kind; /* 0-none, 1-avc, 2-hevc, 3-vvc */
    union {
        struct mpeg4_avc_t avc;
        struct mpeg4_hevc_t hevc;
        struct mpeg4_vvc_t vvc;
    } c;
} zm_mov_track_state;

typedef struct zm_mov_reader {
    zm_reader base;
    zm_io io;
    struct mov_reader_t *reader;
    struct mov_buffer_t buffer;
    zm_mov_track_state states[ZM_MOV_MAX_TRACKS];
    zm_track tracks[ZM_MOV_MAX_TRACKS];
    size_t track_count;
    uint8_t *sample;
    size_t sample_cap;
    uint32_t sample_track;
    size_t sample_bytes;
    int64_t sample_pts;
    int64_t sample_dts;
    int sample_flags;
    uint8_t *conv;
    size_t conv_cap;
} zm_mov_reader;

static int mov_io_read(void *param, void *data, uint64_t bytes)
{
    zm_mov_reader *r = (zm_mov_reader *)param;
    int n;

    if (!r || !r->io.read) {
        return -1;
    }
    n = r->io.read(r->io.ctx, data, (size_t)bytes);
    return n == (int)bytes ? 0 : -1;
}

static int mov_io_write(void *param, const void *data, uint64_t bytes)
{
    (void)param;
    (void)data;
    (void)bytes;
    return -1;
}

static int mov_io_seek(void *param, int64_t offset)
{
    zm_mov_reader *r = (zm_mov_reader *)param;

    if (!r || !r->io.seek) {
        return -1;
    }
    return r->io.seek(r->io.ctx, offset, offset >= 0 ? SEEK_SET : SEEK_END) == 0 ? 0 : -1;
}

static int64_t mov_io_tell(void *param)
{
    zm_mov_reader *r = (zm_mov_reader *)param;
    return (r && r->io.tell) ? r->io.tell(r->io.ctx) : -1;
}

static int mov_track_find(zm_mov_reader *r, uint32_t track_id)
{
    size_t i;

    for (i = 0; i < r->track_count; ++i) {
        if (r->states[i].track_id == track_id) {
            return (int)i;
        }
    }
    return -1;
}

static int mov_track_add(zm_mov_reader *r, uint32_t track_id)
{
    size_t i;

    if (r->track_count >= ZM_MOV_MAX_TRACKS) {
        return -1;
    }
    i = r->track_count++;
    memset(&r->states[i], 0, sizeof(r->states[i]));
    r->states[i].track_id = track_id;
    zm_track_init(&r->tracks[i]);
    r->tracks[i].track_id = track_id;
    r->tracks[i].timebase.num = 1;
    r->tracks[i].timebase.den = 1000;
    return (int)i;
}

static int mov_track_get(zm_mov_reader *r, uint32_t track_id)
{
    int i = mov_track_find(r, track_id);
    return i >= 0 ? i : mov_track_add(r, track_id);
}

static void mov_on_video(void *param, uint32_t track, uint8_t object, int width, int height,
                         const void *extra, size_t bytes)
{
    zm_mov_reader *r = (zm_mov_reader *)param;
    zm_mov_track_state *st;
    zm_track *t;
    int i = mov_track_get(r, track);

    if (i < 0) {
        return;
    }
    st = &r->states[i];
    t = &r->tracks[i];
    t->codec = zm_codec_from_mov(object);
    t->kind = ZM_KIND_VIDEO;
    t->width = width;
    t->height = height;
    t->clock_hz = 90000;
    if (!extra || bytes == 0) {
        return;
    }
    if (t->codec == ZM_CODEC_H264) {
        memset(&st->c.avc, 0, sizeof(st->c.avc));
        if (mpeg4_avc_decoder_configuration_record_load(extra, bytes, &st->c.avc) > 0) {
            st->codec_kind = 1;
            zm_track_set_config(t, ZM_CONFIG_AVCC, extra, bytes);
        }
    } else if (t->codec == ZM_CODEC_H265) {
        memset(&st->c.hevc, 0, sizeof(st->c.hevc));
        if (mpeg4_hevc_decoder_configuration_record_load(extra, bytes, &st->c.hevc) > 0) {
            st->codec_kind = 2;
            zm_track_set_config(t, ZM_CONFIG_HVCC, extra, bytes);
        }
    } else if (t->codec == ZM_CODEC_H266) {
        memset(&st->c.vvc, 0, sizeof(st->c.vvc));
        if (mpeg4_vvc_decoder_configuration_record_load(extra, bytes, &st->c.vvc) > 0) {
            st->codec_kind = 3;
            zm_track_set_config(t, ZM_CONFIG_VVCC, extra, bytes);
        }
    } else if (t->codec == ZM_CODEC_AV1) {
        zm_track_set_config(t, ZM_CONFIG_AV1C, extra, bytes);
    }
}

static void mov_on_audio(void *param, uint32_t track, uint8_t object, int channels,
                         int bits_per_sample, int sample_rate, const void *extra, size_t bytes)
{
    zm_mov_reader *r = (zm_mov_reader *)param;
    zm_track *t;
    int i = mov_track_get(r, track);

    if (i < 0) {
        return;
    }
    t = &r->tracks[i];
    t->codec = zm_codec_from_mov(object);
    t->kind = ZM_KIND_AUDIO;
    t->channels = channels;
    t->bits_per_sample = bits_per_sample;
    t->sample_rate = sample_rate;
    t->clock_hz = sample_rate;
    if (!extra || bytes == 0) {
        return;
    }
    if (t->codec == ZM_CODEC_AAC) {
        zm_track_set_config(t, ZM_CONFIG_ASC, extra, bytes);
    } else if (t->codec == ZM_CODEC_OPUS) {
        zm_track_set_config(t, ZM_CONFIG_OPUS_HEAD, extra, bytes);
    }
}

static void mov_on_subtitle(void *param, uint32_t track, uint8_t object, const void *extra,
                            size_t bytes)
{
    (void)param;
    (void)track;
    (void)object;
    (void)extra;
    (void)bytes;
}

static void *mov_onread2(void *param, uint32_t track, size_t bytes, int64_t pts, int64_t dts,
                         int flags)
{
    zm_mov_reader *r = (zm_mov_reader *)param;

    if (bytes > r->sample_cap) {
        uint8_t *p = (uint8_t *)realloc(r->sample, bytes);
        if (!p) {
            return NULL;
        }
        r->sample = p;
        r->sample_cap = bytes;
    }
    r->sample_track = track;
    r->sample_bytes = bytes;
    r->sample_pts = pts;
    r->sample_dts = dts;
    r->sample_flags = flags;
    return r->sample;
}

static int mov_ensure_conv(zm_mov_reader *r, size_t cap)
{
    if (cap <= r->conv_cap) {
        return 0;
    }
    {
        uint8_t *p = (uint8_t *)realloc(r->conv, cap);
        if (!p) {
            return -1;
        }
        r->conv = p;
        r->conv_cap = cap;
    }
    return 0;
}

static int mov_convert(zm_mov_reader *r, zm_mov_track_state *st, int kind, const uint8_t *data,
                       size_t len, const uint8_t **out, size_t *out_len)
{
    size_t cap = len + 4096;
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        int n;
        if (mov_ensure_conv(r, cap) != 0) {
            return -1;
        }
        if (kind == 1) {
            n = h264_mp4toannexb(&st->c.avc, data, len, r->conv, cap);
        } else if (kind == 2) {
            n = h265_mp4toannexb(&st->c.hevc, data, len, r->conv, cap);
        } else {
            n = h266_mp4toannexb(&st->c.vvc, data, len, r->conv, cap);
        }
        if (n > 0) {
            *out = r->conv;
            *out_len = (size_t)n;
            return 0;
        }
        cap *= 2;
    }
    return -1;
}

static int mov_ops_read(zm_reader *base, zm_packet *out)
{
    zm_mov_reader *r = base ? (zm_mov_reader *)base->impl : NULL;
    const uint8_t *data;
    size_t len;
    uint8_t es_format;
    int rc;
    int i;

    if (!r || !r->reader || !out) {
        return ZM_ERR_INVALID;
    }
    /* 跳过未登记的轨道（字幕/时间码/附件）：它们不是 EOF。 */
    for (;;) {
        rc = mov_reader_read2(r->reader, mov_onread2, r);
        if (rc <= 0) {
            return rc;
        }
        i = mov_track_find(r, r->sample_track);
        if (i >= 0) {
            break;
        }
    }

    data = r->sample;
    len = r->sample_bytes;
    es_format = ZM_ES_RAW;
    if (r->states[i].codec_kind == 1) {
        if (mov_convert(r, &r->states[i], 1, data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
        es_format = ZM_ES_ANNEXB;
    } else if (r->states[i].codec_kind == 2) {
        if (mov_convert(r, &r->states[i], 2, data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
        es_format = ZM_ES_ANNEXB;
    } else if (r->states[i].codec_kind == 3) {
        if (mov_convert(r, &r->states[i], 3, data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
        es_format = ZM_ES_ANNEXB;
    } else if (r->tracks[i].codec == ZM_CODEC_H264 || r->tracks[i].codec == ZM_CODEC_H265 ||
               r->tracks[i].codec == ZM_CODEC_H266) {
        es_format = ZM_ES_LENGTH_PREFIXED;
    }

    zm_packet_init(out);
    if (zm_mem_dup(data, len, &out->mem) != ZM_OK) {
        return ZM_ERR_NOMEM;
    }
    out->codec = r->tracks[i].codec;
    out->kind = r->tracks[i].kind;
    out->track_id = r->sample_track;
    out->pts_ms = r->sample_pts;
    out->dts_ms = r->sample_dts;
    out->flags = (r->sample_flags & MOV_AV_FLAG_KEYFREAME) ? ZM_PKT_KEY : 0;
    out->es_format = es_format;
    return 1;
}

static int mov_reader_input(zm_reader *base, const void *buf, size_t len)
{
    (void)base;
    (void)buf;
    (void)len;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mov_reader_read_raw(zm_reader *base, zm_raw_unit *out)
{
    (void)base;
    (void)out;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mov_reader_seek_impl(zm_reader *base, int64_t *ms)
{
    zm_mov_reader *r = base ? (zm_mov_reader *)base->impl : NULL;

    if (!r || !r->reader || !ms) {
        return ZM_ERR_INVALID;
    }
    return mov_reader_seek(r->reader, ms) == 0 ? ZM_OK : ZM_ERR_INVALID;
}

static const zm_track *mov_reader_tracks(zm_reader *base, size_t *count)
{
    zm_mov_reader *r = base ? (zm_mov_reader *)base->impl : NULL;

    if (!r) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    if (count) {
        *count = r->track_count;
    }
    return r->tracks;
}

static void mov_reader_close(zm_reader *base)
{
    zm_mov_reader *r = base ? (zm_mov_reader *)base->impl : NULL;
    size_t i;

    if (!r) {
        return;
    }
    if (r->reader) {
        mov_reader_destroy(r->reader);
    }
    for (i = 0; i < r->track_count; ++i) {
        zm_track_clear(&r->tracks[i]);
    }
    free(r->sample);
    free(r->conv);
    free(r);
}

static const zm_reader_ops k_mov_reader_ops = {
    .input = mov_reader_input,
    .read = mov_ops_read,
    .read_raw = mov_reader_read_raw,
    .seek = mov_reader_seek_impl,
    .tracks = mov_reader_tracks,
    .close = mov_reader_close,
};

zm_reader *zm_mov_reader_open(const zm_io *io, const zm_reader_cfg *cfg)
{
    zm_mov_reader *r;
    struct mov_reader_trackinfo_t info;

    (void)cfg;
    if (!io || !io->read || !io->seek || !io->tell) {
        return NULL;
    }
    r = (zm_mov_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->io = *io;
    r->buffer.read = mov_io_read;
    r->buffer.write = mov_io_write;
    r->buffer.seek = mov_io_seek;
    r->buffer.tell = mov_io_tell;
    r->reader = mov_reader_create(&r->buffer, r);
    if (!r->reader) {
        free(r);
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.onvideo = mov_on_video;
    info.onaudio = mov_on_audio;
    info.onsubtitle = mov_on_subtitle;
    if (mov_reader_getinfo(r->reader, &info, r) != 0) {
        mov_reader_destroy(r->reader);
        for (size_t i = 0; i < r->track_count; ++i) {
            zm_track_clear(&r->tracks[i]);
        }
        free(r);
        return NULL;
    }
    r->base.ops = &k_mov_reader_ops;
    r->base.impl = r;
    return &r->base;
}
