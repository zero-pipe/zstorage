#include "zm/format/mkv.h"

#include "zm/core/codec.h"

#include "mkv-format.h"
#include "mkv-writer.h"

#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdlib.h>
#include <string.h>

#define ZM_MKV_WRITER_MAX_TRACKS 8

typedef struct zm_mkv_wtrack {
    uint32_t track_id;
    zm_kind kind;
    zm_codec_id codec;
    int mkv_track;
    int codec_kind; /* 0-none, 1-avc, 2-hevc, 3-vvc */
    union {
        struct mpeg4_avc_t avc;
        struct mpeg4_hevc_t hevc;
        struct mpeg4_vvc_t vvc;
    } c;
} zm_mkv_wtrack;

typedef struct zm_mkv_writer {
    zm_writer base;
    zm_sink sink;
    struct mkv_buffer_t buffer;
    mkv_writer_t *mkv;
    zm_mkv_wtrack tracks[ZM_MKV_WRITER_MAX_TRACKS];
    size_t track_count;
    uint8_t *conv;
    size_t conv_cap;
} zm_mkv_writer;

static int mkv_w_io_read(void *param, void *data, uint64_t bytes)
{
    (void)param;
    (void)data;
    (void)bytes;
    return -1;
}

static int mkv_w_io_write(void *param, const void *data, uint64_t bytes)
{
    zm_mkv_writer *w = (zm_mkv_writer *)param;

    if (!w || !data || bytes == 0) {
        return 0;
    }
    return w->sink.write(w->sink.ctx, data, (size_t)bytes) < 0 ? -1 : 0;
}

static int mkv_w_io_seek(void *param, int64_t offset)
{
    zm_mkv_writer *w = (zm_mkv_writer *)param;
    return (w && w->sink.seek) ? (w->sink.seek(w->sink.ctx, offset) == 0 ? 0 : -1) : -1;
}

static int64_t mkv_w_io_tell(void *param)
{
    zm_mkv_writer *w = (zm_mkv_writer *)param;
    int64_t off = 0;

    if (!w || !w->sink.tell) {
        return -1;
    }
    return w->sink.tell(w->sink.ctx, &off) == 0 ? off : -1;
}

static int mkv_writer_pick(zm_mkv_writer *w, const zm_packet *pkt)
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

static int mkv_writer_ensure_conv(zm_mkv_writer *w, size_t cap)
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

static int mkv_writer_convert(zm_mkv_writer *w, zm_mkv_wtrack *t, const uint8_t *data, size_t len,
                              const uint8_t **out, size_t *out_len)
{
    size_t cap = len + 4096;
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        int n;
        int vcl = 0;
        int update = 0;
        if (mkv_writer_ensure_conv(w, cap) != 0) {
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

static size_t mkv_writer_strip_adts(const uint8_t *data, size_t len)
{
    if (len >= 7 && data[0] == 0xff && (data[1] & 0xf0) == 0xf0) {
        return (data[1] & 0x01) ? 7u : 9u;
    }
    return 0;
}

static int mkv_ops_write(zm_writer *base, const zm_packet *pkt)
{
    zm_mkv_writer *w = base ? (zm_mkv_writer *)base->impl : NULL;
    const uint8_t *data;
    size_t len;
    int idx;
    int flags = 0;

    if (!w || !w->mkv || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        return ZM_OK;
    }
    idx = mkv_writer_pick(w, pkt);
    if (idx < 0) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    data = pkt->mem.data;
    len = pkt->mem.len;
    if (pkt->es_format == ZM_ES_ADTS) {
        size_t hdr = mkv_writer_strip_adts(data, len);
        data += hdr;
        len -= hdr;
    }
    if ((w->tracks[idx].codec_kind == 1 || w->tracks[idx].codec_kind == 2 ||
         w->tracks[idx].codec_kind == 3) &&
        pkt->es_format != ZM_ES_LENGTH_PREFIXED) {
        if (mkv_writer_convert(w, &w->tracks[idx], data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
    }
    if (pkt->flags & ZM_PKT_KEY) {
        flags |= MKV_FLAGS_KEYFRAME;
    }
    return mkv_writer_write(w->mkv, w->tracks[idx].mkv_track, data, len, pkt->pts_ms, pkt->dts_ms,
                            flags) == 0
               ? ZM_OK
               : ZM_ERR_INVALID;
}

static int mkv_writer_write_raw(zm_writer *base, const zm_raw_unit *unit)
{
    (void)base;
    (void)unit;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mkv_writer_flush(zm_writer *base, int force_boundary)
{
    (void)base;
    (void)force_boundary;
    return ZM_OK;
}

static int mkv_writer_finish(zm_writer *base)
{
    (void)base;
    return ZM_OK;
}

static void mkv_writer_close(zm_writer *base)
{
    zm_mkv_writer *w = base ? (zm_mkv_writer *)base->impl : NULL;

    if (!w) {
        return;
    }
    if (w->mkv) {
        mkv_writer_destroy(w->mkv);
    }
    free(w->conv);
    free(w);
}

static const zm_writer_ops k_mkv_writer_ops = {
    .write = mkv_ops_write,
    .write_raw = mkv_writer_write_raw,
    .flush = mkv_writer_flush,
    .finish = mkv_writer_finish,
    .close = mkv_writer_close,
};

zm_writer *zm_mkv_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg)
{
    zm_mkv_writer *w;
    size_t i;

    if (!sink || !sink->write || !cfg) {
        return NULL;
    }
    w = (zm_mkv_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->sink = *sink;
    w->buffer.read = mkv_w_io_read;
    w->buffer.write = mkv_w_io_write;
    w->buffer.seek = mkv_w_io_seek;
    w->buffer.tell = mkv_w_io_tell;
    w->mkv = mkv_writer_create(&w->buffer, w, 0);
    if (!w->mkv) {
        free(w);
        return NULL;
    }

    for (i = 0; i < cfg->track_count && w->track_count < ZM_MKV_WRITER_MAX_TRACKS; ++i) {
        const zm_track *src = &cfg->tracks[i];
        const zm_codec_desc *desc = zm_codec_get(src->codec);
        zm_mkv_wtrack *dst = &w->tracks[w->track_count];
        int track;

        if (!desc || desc->map.mkv_codec == 0) {
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
            track = mkv_writer_add_video(w->mkv, (enum mkv_codec_t)desc->map.mkv_codec, src->width,
                                         src->height, src->config.data, src->config.len);
        } else if (src->kind == ZM_KIND_AUDIO) {
            track = mkv_writer_add_audio(w->mkv, (enum mkv_codec_t)desc->map.mkv_codec,
                                         src->channels, src->bits_per_sample, src->sample_rate,
                                         src->config.data, src->config.len);
        } else {
            continue;
        }
        if (track < 0) {
            continue;
        }
        dst->mkv_track = track;
        ++w->track_count;
    }
    if (w->track_count == 0) {
        mkv_writer_destroy(w->mkv);
        free(w);
        return NULL;
    }
    w->base.ops = &k_mkv_writer_ops;
    w->base.impl = w;
    return &w->base;
}
