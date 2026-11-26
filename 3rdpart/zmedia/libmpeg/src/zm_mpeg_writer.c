#include "zm/format/mpeg.h"

#include "zm/core/codec.h"

#include "mpeg-proto.h"
#include "mpeg-ps.h"
#include "mpeg-ts.h"

#include <stdlib.h>
#include <string.h>

typedef struct zm_mpeg_writer {
    zm_writer base;
    zm_sink sink;
    int is_ps;
    void *ts;
    struct ps_muxer_t *ps;
    int stream_video;
    int stream_audio;
    uint8_t asc[64];
    size_t asc_len;
} zm_mpeg_writer;

static void *mpeg_writer_alloc(void *param, size_t bytes)
{
    (void)param;
    return malloc(bytes);
}

static void mpeg_writer_free(void *param, void *packet)
{
    (void)param;
    free(packet);
}

static int mpeg_writer_write_ts(void *param, const void *packet, size_t bytes)
{
    zm_mpeg_writer *w = (zm_mpeg_writer *)param;

    if (!w || !packet || bytes == 0) {
        return 0;
    }
    return w->sink.write(w->sink.ctx, packet, bytes) < 0 ? -1 : 0;
}

static int mpeg_writer_write_ps(void *param, int stream, void *packet, size_t bytes)
{
    zm_mpeg_writer *w = (zm_mpeg_writer *)param;

    (void)stream;
    if (!w || !packet || bytes == 0) {
        return 0;
    }
    return w->sink.write(w->sink.ctx, packet, bytes) < 0 ? -1 : 0;
}

static size_t mpeg_build_adts(const uint8_t *asc, size_t asc_len, const uint8_t *raw,
                              size_t raw_len, uint8_t *out, size_t cap)
{
    int profile;
    int sr_idx;
    int channels;
    size_t total = 7 + raw_len;

    if (!asc || asc_len < 2 || !out || total > cap) {
        return 0;
    }
    profile = (asc[0] >> 3) & 0x1f;
    sr_idx = ((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01);
    channels = (asc[1] >> 3) & 0x0f;
    if (profile <= 0) {
        profile = 2;
    }
    out[0] = 0xff;
    out[1] = 0xf1;
    out[2] = (uint8_t)(((profile - 1) << 6) | (sr_idx << 2) | ((channels >> 2) & 1));
    out[3] = (uint8_t)(((channels & 3) << 6) | ((total >> 11) & 0x03));
    out[4] = (uint8_t)((total >> 3) & 0xff);
    out[5] = (uint8_t)(((total & 7) << 5) | 0x1f);
    out[6] = 0xfc;
    memcpy(out + 7, raw, raw_len);
    return total;
}

static int mpeg_ensure_stream(zm_mpeg_writer *w, zm_kind kind, zm_codec_id codec)
{
    const zm_codec_desc *desc = zm_codec_get(codec);
    int stream;

    if (!w || !desc) {
        return ZM_ERR_INVALID;
    }
    if (desc->map.psi == 0) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    if (kind == ZM_KIND_VIDEO) {
        if (w->stream_video > 0) {
            return w->stream_video;
        }
        stream = w->is_ps ? ps_muxer_add_stream(w->ps, desc->map.psi, NULL, 0)
                          : mpeg_ts_add_stream(w->ts, desc->map.psi, NULL, 0);
        if (stream > 0) {
            w->stream_video = stream;
        }
        return stream > 0 ? stream : ZM_ERR_NOT_SUPPORTED;
    }
    if (kind == ZM_KIND_AUDIO) {
        if (w->stream_audio > 0) {
            return w->stream_audio;
        }
        stream = w->is_ps ? ps_muxer_add_stream(w->ps, desc->map.psi, NULL, 0)
                          : mpeg_ts_add_stream(w->ts, desc->map.psi, NULL, 0);
        if (stream > 0) {
            w->stream_audio = stream;
        }
        return stream > 0 ? stream : ZM_ERR_NOT_SUPPORTED;
    }
    return ZM_ERR_NOT_SUPPORTED;
}

static int mpeg_ops_write(zm_writer *base, const zm_packet *pkt)
{
    zm_mpeg_writer *w = base ? (zm_mpeg_writer *)base->impl : NULL;
    int stream;
    int flags = 0;
    int64_t pts_90k;
    int64_t dts_90k;
    uint8_t *adts = NULL;
    const uint8_t *data;
    size_t bytes;
    int rc;

    if (!w || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        if (pkt->codec == ZM_CODEC_AAC && pkt->mem.len <= sizeof(w->asc)) {
            memcpy(w->asc, pkt->mem.data, pkt->mem.len);
            w->asc_len = pkt->mem.len;
        }
        return ZM_OK;
    }

    stream = mpeg_ensure_stream(w, pkt->kind, pkt->codec);
    if (stream <= 0) {
        return stream;
    }
    data = pkt->mem.data;
    bytes = pkt->mem.len;
    if (pkt->codec == ZM_CODEC_AAC) {
        size_t need = 7 + bytes;
        adts = (uint8_t *)malloc(need);
        if (!adts) {
            return ZM_ERR_NOMEM;
        }
        if (mpeg_build_adts(w->asc, w->asc_len, data, bytes, adts, need) == 0) {
            free(adts);
            return ZM_ERR_NOT_SUPPORTED;
        }
        data = adts;
        bytes = need;
    }

    dts_90k = pkt->dts_ms * 90;
    pts_90k = pkt->pts_ms > pkt->dts_ms ? pkt->pts_ms * 90 : dts_90k;
    if (pkt->flags & ZM_PKT_KEY) {
        flags |= MPEG_FLAG_IDR_FRAME;
    }
    if (w->is_ps) {
        rc = ps_muxer_input(w->ps, stream, flags, pts_90k, dts_90k, data, bytes);
    } else {
        rc = mpeg_ts_write(w->ts, stream, flags, pts_90k, dts_90k, data, bytes);
    }
    free(adts);
    return rc >= 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int mpeg_writer_write_raw(zm_writer *base, const zm_raw_unit *unit)
{
    (void)base;
    (void)unit;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mpeg_writer_flush(zm_writer *base, int force_boundary)
{
    (void)base;
    (void)force_boundary;
    return ZM_OK;
}

static int mpeg_writer_finish(zm_writer *base)
{
    (void)base;
    return ZM_OK;
}

static void mpeg_writer_close(zm_writer *base)
{
    zm_mpeg_writer *w = base ? (zm_mpeg_writer *)base->impl : NULL;

    if (!w) {
        return;
    }
    if (w->ts) {
        mpeg_ts_destroy(w->ts);
    }
    if (w->ps) {
        ps_muxer_destroy(w->ps);
    }
    free(w);
}

static const zm_writer_ops k_mpeg_writer_ops = {
    .write = mpeg_ops_write,
    .write_raw = mpeg_writer_write_raw,
    .flush = mpeg_writer_flush,
    .finish = mpeg_writer_finish,
    .close = mpeg_writer_close,
};

zm_writer *zm_mpeg_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg)
{
    zm_mpeg_writer *w;
    size_t i;

    if (!sink || !sink->write || !cfg) {
        return NULL;
    }
    if (cfg->format != ZM_FMT_MPEG_TS && cfg->format != ZM_FMT_MPEG_PS) {
        return NULL;
    }
    w = (zm_mpeg_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->sink = *sink;
    w->is_ps = cfg->format == ZM_FMT_MPEG_PS;
    w->stream_video = -1;
    w->stream_audio = -1;
    if (w->is_ps) {
        struct ps_muxer_func_t func;
        func.alloc = mpeg_writer_alloc;
        func.free = mpeg_writer_free;
        func.write = mpeg_writer_write_ps;
        w->ps = ps_muxer_create(&func, w);
    } else {
        struct mpeg_ts_func_t func;
        func.alloc = mpeg_writer_alloc;
        func.free = mpeg_writer_free;
        func.write = mpeg_writer_write_ts;
        w->ts = mpeg_ts_create(&func, w);
    }
    if (!w->ps && !w->ts) {
        free(w);
        return NULL;
    }
    if (cfg->tracks) {
        for (i = 0; i < cfg->track_count && i < 2; ++i) {
            if (cfg->tracks[i].kind == ZM_KIND_AUDIO && cfg->tracks[i].codec == ZM_CODEC_AAC &&
                cfg->tracks[i].config.fmt == ZM_CONFIG_ASC &&
                cfg->tracks[i].config.len <= sizeof(w->asc)) {
                memcpy(w->asc, cfg->tracks[i].config.data, cfg->tracks[i].config.len);
                w->asc_len = cfg->tracks[i].config.len;
            }
        }
    }
    w->base.ops = &k_mpeg_writer_ops;
    w->base.impl = w;
    return &w->base;
}
