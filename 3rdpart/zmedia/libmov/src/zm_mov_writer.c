#include "zm/format/mov.h"

#include "zm/core/codec.h"

#include "mov-format.h"
#include "mp4-writer.h"

#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdlib.h>
#include <string.h>

#define ZM_MOV_WRITER_MAX_TRACKS 8

typedef struct zm_mov_wtrack {
    uint32_t track_id;
    zm_kind kind;
    zm_codec_id codec;
    int mp4_track;
    int codec_kind; /* 0-none, 1-avc, 2-hevc, 3-vvc */
    union {
        struct mpeg4_avc_t avc;
        struct mpeg4_hevc_t hevc;
        struct mpeg4_vvc_t vvc;
    } c;
} zm_mov_wtrack;

typedef struct zm_mov_writer {
    zm_writer base;
    zm_sink sink;
    struct mov_buffer_t buffer;
    struct mp4_writer_t *mp4;
    int is_fmp4;
    zm_mov_wtrack tracks[ZM_MOV_WRITER_MAX_TRACKS];
    size_t track_count;
    uint8_t *conv;
    size_t conv_cap;
} zm_mov_writer;

static int mov_w_io_read(void *param, void *data, uint64_t bytes)
{
    (void)param;
    (void)data;
    (void)bytes;
    return -1;
}

static int mov_w_io_write(void *param, const void *data, uint64_t bytes)
{
    zm_mov_writer *w = (zm_mov_writer *)param;

    if (!w || !data || bytes == 0) {
        return 0;
    }
    return w->sink.write(w->sink.ctx, data, (size_t)bytes) < 0 ? -1 : 0;
}

static int mov_w_io_seek(void *param, int64_t offset)
{
    zm_mov_writer *w = (zm_mov_writer *)param;
    return (w && w->sink.seek) ? (w->sink.seek(w->sink.ctx, offset) == 0 ? 0 : -1) : -1;
}

static int64_t mov_w_io_tell(void *param)
{
    zm_mov_writer *w = (zm_mov_writer *)param;
    int64_t off = 0;

    if (!w || !w->sink.tell) {
        return -1;
    }
    return w->sink.tell(w->sink.ctx, &off) == 0 ? off : -1;
}

static int mov_writer_pick(zm_mov_writer *w, const zm_packet *pkt)
{
    size_t i;

    for (i = 0; i < w->track_count; ++i) {
        if (pkt->track_id && w->tracks[i].track_id == pkt->track_id) {
            return (int)i;
        }
    }
    for (i = 0; i < w->track_count; ++i) {
        if (w->tracks[i].kind == pkt->kind) {
            return (int)i;
        }
    }
    return -1;
}

static int mov_writer_ensure_conv(zm_mov_writer *w, size_t cap)
{
    if (cap <= w->conv_cap) {
        return 0;
    }
    {
        uint8_t *p = (uint8_t *)realloc(w->conv, cap);
        if (!p) {
            return -1;
        }
        w->conv = p;
        w->conv_cap = cap;
    }
    return 0;
}

static int mov_writer_convert(zm_mov_writer *w, zm_mov_wtrack *t, const uint8_t *data, size_t len,
                              const uint8_t **out, size_t *out_len)
{
    size_t cap = len + 4096;
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        int n;
        int vcl = 0;
        int update = 0;
        if (mov_writer_ensure_conv(w, cap) != 0) {
            return -1;
        }
        if (t->codec_kind == 1) {
            n = h264_annexbtomp4(&t->c.avc, data, len, w->conv, cap, &vcl, &update);
        } else if (t->codec_kind == 2) {
            n = h265_annexbtomp4(&t->c.hevc, data, len, w->conv, cap, &vcl, &update);
        } else {
            n = h266_annexbtomp4(&t->c.vvc, data, len, w->conv, cap, &vcl, &update);
        }
        if (n > 0) {
            *out = w->conv;
            *out_len = (size_t)n;
            return 0;
        }
        cap *= 2;
    }
    return -1;
}

static size_t mov_writer_strip_adts(const uint8_t *data, size_t len)
{
    if (len >= 7 && data[0] == 0xff && (data[1] & 0xf0) == 0xf0) {
        return (data[1] & 0x01) ? 7u : 9u;
    }
    return 0;
}

static int mov_ops_write(zm_writer *base, const zm_packet *pkt)
{
    zm_mov_writer *w = base ? (zm_mov_writer *)base->impl : NULL;
    const uint8_t *data;
    size_t len;
    int idx;
    int flags = 0;
    int rc;

    if (!w || !w->mp4 || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        return ZM_OK;
    }
    idx = mov_writer_pick(w, pkt);
    if (idx < 0) {
        return ZM_ERR_NOT_SUPPORTED;
    }

    data = pkt->mem.data;
    len = pkt->mem.len;
    if (pkt->es_format == ZM_ES_ADTS) {
        size_t hdr = mov_writer_strip_adts(data, len);
        data += hdr;
        len -= hdr;
    }
    if ((w->tracks[idx].codec_kind == 1 || w->tracks[idx].codec_kind == 2 ||
         w->tracks[idx].codec_kind == 3) &&
        pkt->es_format != ZM_ES_LENGTH_PREFIXED) {
        if (mov_writer_convert(w, &w->tracks[idx], data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
    }
    if (pkt->flags & ZM_PKT_KEY) {
        flags |= MOV_AV_FLAG_KEYFREAME;
    }
    rc = mp4_writer_write(w->mp4, w->tracks[idx].mp4_track, data, len, pkt->pts_ms, pkt->dts_ms,
                          flags);
    return rc == 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int mov_writer_write_raw(zm_writer *base, const zm_raw_unit *unit)
{
    (void)base;
    (void)unit;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mov_writer_flush(zm_writer *base, int force_boundary)
{
    zm_mov_writer *w = base ? (zm_mov_writer *)base->impl : NULL;

    if (!w || !w->mp4) {
        return ZM_ERR_INVALID;
    }
    if (w->is_fmp4 && force_boundary) {
        return mp4_writer_save_segment(w->mp4) == 0 ? ZM_OK : ZM_ERR_INVALID;
    }
    return ZM_OK;
}

static int mov_writer_finish(zm_writer *base)
{
    zm_mov_writer *w = base ? (zm_mov_writer *)base->impl : NULL;

    if (!w || !w->mp4) {
        return ZM_ERR_INVALID;
    }
    if (w->is_fmp4) {
        return mp4_writer_save_segment(w->mp4) == 0 ? ZM_OK : ZM_ERR_INVALID;
    }
    return ZM_OK;
}

static void mov_writer_close(zm_writer *base)
{
    zm_mov_writer *w = base ? (zm_mov_writer *)base->impl : NULL;

    if (!w) {
        return;
    }
    if (w->mp4) {
        mp4_writer_destroy(w->mp4);
    }
    free(w->conv);
    free(w);
}

static const zm_writer_ops k_mov_writer_ops = {
    .write = mov_ops_write,
    .write_raw = mov_writer_write_raw,
    .flush = mov_writer_flush,
    .finish = mov_writer_finish,
    .close = mov_writer_close,
};

zm_writer *zm_mov_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg)
{
    zm_mov_writer *w;
    int is_fmp4;
    size_t i;

    if (!sink || !sink->write || !cfg) {
        return NULL;
    }
    if (cfg->format != ZM_FMT_MOV && cfg->format != ZM_FMT_FMP4) {
        return NULL;
    }
    is_fmp4 = cfg->format == ZM_FMT_FMP4;
    if (!is_fmp4 && (!sink->seek || !sink->tell)) {
        return NULL;
    }

    w = (zm_mov_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->sink = *sink;
    w->is_fmp4 = is_fmp4;
    w->buffer.read = mov_w_io_read;
    w->buffer.write = mov_w_io_write;
    w->buffer.seek = mov_w_io_seek;
    w->buffer.tell = mov_w_io_tell;
    w->mp4 = mp4_writer_create(is_fmp4, &w->buffer, w, 0);
    if (!w->mp4 || (!w->mp4->mov && !w->mp4->fmp4)) {
        if (w->mp4) {
            mp4_writer_destroy(w->mp4);
        }
        free(w);
        return NULL;
    }

    for (i = 0; i < cfg->track_count && w->track_count < ZM_MOV_WRITER_MAX_TRACKS; ++i) {
        const zm_track *src = &cfg->tracks[i];
        const zm_codec_desc *desc = zm_codec_get(src->codec);
        zm_mov_wtrack *dst = &w->tracks[w->track_count];
        int track;

        if (!desc || desc->map.mov_object == 0) {
            continue;
        }
        memset(dst, 0, sizeof(*dst));
        dst->track_id = src->track_id;
        dst->kind = src->kind;
        dst->codec = src->codec;

        if (src->kind == ZM_KIND_VIDEO) {
            if (src->codec == ZM_CODEC_H264) {
                if (src->config.fmt != ZM_CONFIG_AVCC ||
                    mpeg4_avc_decoder_configuration_record_load(src->config.data, src->config.len,
                                                                &dst->c.avc) <= 0) {
                    continue;
                }
                dst->codec_kind = 1;
            } else if (src->codec == ZM_CODEC_H265) {
                if (src->config.fmt != ZM_CONFIG_HVCC ||
                    mpeg4_hevc_decoder_configuration_record_load(src->config.data, src->config.len,
                                                                 &dst->c.hevc) <= 0) {
                    continue;
                }
                dst->codec_kind = 2;
            } else if (src->codec == ZM_CODEC_H266) {
                if (src->config.fmt != ZM_CONFIG_VVCC ||
                    mpeg4_vvc_decoder_configuration_record_load(src->config.data, src->config.len,
                                                                &dst->c.vvc) <= 0) {
                    continue;
                }
                dst->codec_kind = 3;
            }
            track = mp4_writer_add_video(w->mp4, (uint8_t)desc->map.mov_object, src->width,
                                         src->height, src->config.data, src->config.len);
        } else if (src->kind == ZM_KIND_AUDIO) {
            track = mp4_writer_add_audio(w->mp4, (uint8_t)desc->map.mov_object, src->channels,
                                         src->bits_per_sample, src->sample_rate, src->config.data,
                                         src->config.len);
        } else {
            continue;
        }
        if (track < 0) {
            continue;
        }
        dst->mp4_track = track;
        ++w->track_count;
    }
    if (w->track_count == 0) {
        mp4_writer_destroy(w->mp4);
        free(w);
        return NULL;
    }
    w->base.ops = &k_mov_writer_ops;
    w->base.impl = w;
    return &w->base;
}
