#include "zm/format/flv.h"

#include "flv-muxer.h"
#include "flv-writer.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* libflv 已实现但未在公共头声明；这里显式声明，避免 per-codec 适配文件。 */
int flv_muxer_vvc(flv_muxer_t *flv, const void *data, size_t bytes, uint32_t pts, uint32_t dts);

typedef struct zm_flv_writer {
    zm_writer base;
    zm_sink sink;
    int tag_body;
    flv_muxer_t *muxer;
    void *fw;
    int has_video;
    int has_audio;
    zm_flv_tag_cb tag_cb;
    void *tag_user;
    zm_track tracks[2];
    size_t track_count;
} zm_flv_writer;

static uint32_t flv_u32(int64_t v)
{
    if (v <= 0) {
        return 0;
    }
    if (v > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)v;
}

static int flv_on_tag(void *param, int type, const void *data, size_t bytes, uint32_t timestamp)
{
    zm_flv_writer *w = (zm_flv_writer *)param;

    if (!w || !data || bytes == 0) {
        return 0;
    }
    if (w->tag_cb) {
        return w->tag_cb(w->tag_user, type, data, bytes, timestamp);
    }
    if (w->tag_body) {
        return w->sink.write(w->sink.ctx, data, bytes) < 0 ? -1 : 0;
    }
    if (!w->fw) {
        return -1;
    }
    return flv_writer_input(w->fw, type, data, bytes, timestamp) == 0 ? 0 : -1;
}

static int flv_on_write(void *param, const struct flv_vec_t *vec, int n)
{
    zm_flv_writer *w = (zm_flv_writer *)param;
    int i;

    if (!w || !vec) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (vec[i].len > 0 && w->sink.write(w->sink.ctx, vec[i].ptr, (size_t)vec[i].len) < 0) {
            return -1;
        }
    }
    return 0;
}

/* AAC 走手工 FLV AudioTagHeader：reader 已归一化为 raw AAC + ASC。 */
static int flv_emit_aac(zm_flv_writer *w, int seq, const uint8_t *data, size_t len,
                        uint32_t dts)
{
    uint8_t stack[256];
    uint8_t *body = stack;
    size_t total = 2 + len;
    int rc;

    if (!w || !data || len == 0) {
        return -1;
    }
    if (total > sizeof(stack)) {
        body = (uint8_t *)malloc(total);
        if (!body) {
            return -1;
        }
    }
    body[0] = 0xaf;
    body[1] = seq ? 0x00 : 0x01;
    memcpy(body + 2, data, len);
    rc = flv_on_tag(w, 8, body, total, dts);
    if (body != stack) {
        free(body);
    }
    return rc;
}


static size_t flv_append_annexb(uint8_t *buf, size_t cap, size_t off, const uint8_t *data,
                                size_t len)
{
    if (!buf || !data || len == 0 || off + 4 + len > cap) {
        return off;
    }
    buf[off + 0] = 0;
    buf[off + 1] = 0;
    buf[off + 2] = 0;
    buf[off + 3] = 1;
    memcpy(buf + off + 4, data, len);
    return off + 4 + len;
}

static int flv_seed_h264(zm_flv_writer *w, const uint8_t *avcc, size_t len)
{
    struct mpeg4_avc_t avc;
    uint8_t buf[4096];
    size_t off = 0;
    int i;

    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(avcc, len, &avc) <= 0) {
        return -1;
    }
    for (i = 0; i < (int)avc.nb_sps; ++i) {
        off = flv_append_annexb(buf, sizeof(buf), off, avc.sps[i].data, avc.sps[i].bytes);
    }
    for (i = 0; i < (int)avc.nb_pps; ++i) {
        off = flv_append_annexb(buf, sizeof(buf), off, avc.pps[i].data, avc.pps[i].bytes);
    }
    if (off == 0) {
        return -1;
    }
    return flv_muxer_avc(w->muxer, buf, off, 0, 0) == 0 ? 0 : -1;
}

static int flv_seed_h265(zm_flv_writer *w, const uint8_t *hvcc, size_t len)
{
    struct mpeg4_hevc_t hevc;
    uint8_t buf[4096];
    size_t off = 0;
    int i;

    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, len, &hevc) <= 0) {
        return -1;
    }
    for (i = 0; i < (int)hevc.numOfArrays; ++i) {
        off = flv_append_annexb(buf, sizeof(buf), off, hevc.nalu[i].data, hevc.nalu[i].bytes);
    }
    if (off == 0) {
        return -1;
    }
    return flv_muxer_hevc(w->muxer, buf, off, 0, 0) == 0 ? 0 : -1;
}

static int flv_write(zm_writer *base, const zm_packet *pkt)
{
    zm_flv_writer *w = base ? (zm_flv_writer *)base->impl : NULL;
    uint32_t pts;
    uint32_t dts;
    int rc;

    if (!w || !w->muxer || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    pts = flv_u32(pkt->pts_ms);
    dts = flv_u32(pkt->dts_ms);

    if (pkt->flags & ZM_PKT_CONFIG) {
        if (pkt->codec == ZM_CODEC_AAC) {
            return flv_emit_aac(w, 1, pkt->mem.data, pkt->mem.len, dts) == 0 ? ZM_OK : ZM_ERR_IO;
        }
        if (pkt->codec == ZM_CODEC_OPUS) {
            rc = flv_muxer_opus(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
            return rc == 0 ? ZM_OK : ZM_ERR_INVALID;
        }
        if (pkt->codec == ZM_CODEC_H264) {
            (void)flv_seed_h264(w, pkt->mem.data, pkt->mem.len);
            return ZM_OK;
        }
        if (pkt->codec == ZM_CODEC_H265) {
            (void)flv_seed_h265(w, pkt->mem.data, pkt->mem.len);
            return ZM_OK;
        }
        return ZM_OK;
    }

    switch (pkt->codec) {
    case ZM_CODEC_H264:
        rc = flv_muxer_avc(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_H265:
        rc = flv_muxer_hevc(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_H266:
        rc = flv_muxer_vvc(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_AV1:
        rc = flv_muxer_av1(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_AAC:
        rc = flv_emit_aac(w, 0, pkt->mem.data, pkt->mem.len, dts);
        break;
    case ZM_CODEC_OPUS:
        rc = flv_muxer_opus(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_G711A:
        rc = flv_muxer_g711a(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_G711U:
        rc = flv_muxer_g711u(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_MP3:
        rc = flv_muxer_mp3(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_AC3:
        rc = flv_muxer_ac3(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    case ZM_CODEC_EAC3:
        rc = flv_muxer_eac3(w->muxer, pkt->mem.data, pkt->mem.len, pts, dts);
        break;
    default:
        return ZM_ERR_NOT_SUPPORTED;
    }
    return rc == 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int flv_write_raw(zm_writer *base, const zm_raw_unit *unit)
{
    zm_flv_writer *w = base ? (zm_flv_writer *)base->impl : NULL;

    if (!w || !unit || !unit->body.data || unit->body.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (unit->kind == ZM_RAW_FLV_TAG) {
        return w->sink.write(w->sink.ctx, unit->body.data, unit->body.len) < 0 ? ZM_ERR_IO
                                                                               : ZM_OK;
    }
    if (unit->kind == ZM_RAW_FLV_TAG_BODY) {
        return flv_on_tag(w, unit->type, unit->body.data, unit->body.len, unit->dts_ms) == 0
                   ? ZM_OK
                   : ZM_ERR_IO;
    }
    return ZM_ERR_NOT_SUPPORTED;
}

static int flv_flush(zm_writer *base, int force_boundary)
{
    (void)base;
    (void)force_boundary;
    return ZM_OK;
}

static int flv_finish(zm_writer *base)
{
    (void)base;
    return ZM_OK;
}

static void zm_flv_writer_free(zm_flv_writer *w)
{
    if (!w) {
        return;
    }
    if (w->muxer) {
        flv_muxer_destroy(w->muxer);
    }
    if (w->fw) {
        flv_writer_destroy(w->fw);
    }
    zm_track_clear(&w->tracks[0]);
    zm_track_clear(&w->tracks[1]);
    free(w);
}

static void flv_writer_close(zm_writer *base)
{
    zm_flv_writer *w = base ? (zm_flv_writer *)base->impl : NULL;
    zm_flv_writer_free(w);
}

static const zm_writer_ops k_flv_writer_ops = {
    .write = flv_write,
    .write_raw = flv_write_raw,
    .flush = flv_flush,
    .finish = flv_finish,
    .close = flv_writer_close,
};

zm_writer *zm_flv_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg)
{
    zm_flv_writer *w;
    size_t i;
    size_t n;

    if (!sink || !sink->write) {
        return NULL;
    }
    w = (zm_flv_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->sink = *sink;
    w->tag_body = cfg ? cfg->tag_body : 0;
    w->track_count = 2;
    zm_track_init(&w->tracks[0]);
    zm_track_init(&w->tracks[1]);

    if (cfg && cfg->tracks && cfg->track_count) {
        n = cfg->track_count < 2 ? cfg->track_count : 2;
        for (i = 0; i < n; ++i) {
            const zm_track *src = &cfg->tracks[i];
            zm_track *dst = &w->tracks[i];
            dst->track_id = src->track_id;
            dst->codec = src->codec;
            dst->kind = src->kind;
            dst->clock_hz = src->clock_hz;
            dst->width = src->width;
            dst->height = src->height;
            dst->sample_rate = src->sample_rate;
            dst->channels = src->channels;
            dst->bits_per_sample = src->bits_per_sample;
            dst->timebase = src->timebase;
            if (zm_track_set_config(dst, src->config.fmt, src->config.data, src->config.len) !=
                ZM_OK) {
                zm_flv_writer_free(w);
                return NULL;
            }
            if (dst->kind == ZM_KIND_VIDEO) {
                w->has_video = 1;
            } else if (dst->kind == ZM_KIND_AUDIO) {
                w->has_audio = 1;
            }
        }
    }
    if (!w->has_video && !w->has_audio) {
        w->has_video = 1;
        w->has_audio = 1;
    }

    w->muxer = flv_muxer_create(flv_on_tag, w);
    if (!w->muxer) {
        zm_flv_writer_free(w);
        return NULL;
    }
    for (i = 0; i < 2; ++i) {
        if (w->tracks[i].kind == ZM_KIND_VIDEO && w->tracks[i].codec != ZM_CODEC_H264 &&
            w->tracks[i].codec != ZM_CODEC_INVALID) {
            flv_muxer_set_enhanced_rtmp(w->muxer, 1);
            break;
        }
    }

    if (!w->tag_body) {
        w->fw = flv_writer_create2(w->has_audio, w->has_video, flv_on_write, w);
        if (!w->fw) {
            zm_flv_writer_free(w);
            return NULL;
        }
    }

    w->base.ops = &k_flv_writer_ops;
    w->base.impl = w;
    return &w->base;
}

void zm_flv_writer_set_tag_cb(zm_writer *base, zm_flv_tag_cb cb, void *user)
{
    zm_flv_writer *w = base ? (zm_flv_writer *)base->impl : NULL;

    if (!w) {
        return;
    }
    w->tag_cb = cb;
    w->tag_user = user;
}
