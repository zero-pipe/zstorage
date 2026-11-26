#include "zm/transport/rtp.h"

#include "zm/core/codec.h"

#include "rtp-payload.h"

#include <stdlib.h>
#include <string.h>

typedef struct zm_rtp_writer {
    zm_writer base;
    zm_sink sink;
    void *encoder;
    zm_codec_id codec;
    int clock_hz;
    int64_t first_ms;
    int first_ms_set;
} zm_rtp_writer;

static void *rtp_writer_alloc(void *param, int bytes)
{
    (void)param;
    if (bytes <= 0) {
        return NULL;
    }
    return malloc((size_t)bytes);
}

static void rtp_writer_free(void *param, void *packet)
{
    (void)param;
    free(packet);
}

static int rtp_writer_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                             int flags)
{
    zm_rtp_writer *w = (zm_rtp_writer *)param;
    (void)timestamp;
    (void)flags;

    if (!w || !packet || bytes <= 0) {
        return -1;
    }
    return w->sink.write(w->sink.ctx, packet, (size_t)bytes) < 0 ? -1 : 0;
}

static uint32_t rtp_ts_from_ms(zm_rtp_writer *w, int64_t ms)
{
    int64_t delta;
    uint64_t ticks;

    if (!w->first_ms_set) {
        w->first_ms = ms;
        w->first_ms_set = 1;
    }
    delta = ms - w->first_ms;
    if (delta < 0) {
        delta = 0;
    }
    ticks = (uint64_t)delta * (uint64_t)w->clock_hz / 1000u;
    return (uint32_t)ticks;
}

static int rtp_writer_write(zm_writer *base, const zm_packet *pkt)
{
    zm_rtp_writer *w = base ? (zm_rtp_writer *)base->impl : NULL;
    int64_t ts_ms;
    uint32_t ts;
    int rc;

    if (!w || !w->encoder || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        /* RTP payloader 需要参数集内联在 AU 中；config 由上层决定是否注入。 */
        return ZM_OK;
    }
    ts_ms = pkt->kind == ZM_KIND_AUDIO ? pkt->pts_ms : pkt->dts_ms;
    ts = rtp_ts_from_ms(w, ts_ms);
    rc = rtp_payload_encode_input(w->encoder, pkt->mem.data, (int)pkt->mem.len, ts);
    return rc >= 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int rtp_writer_write_raw(zm_writer *base, const zm_raw_unit *unit)
{
    zm_rtp_writer *w = base ? (zm_rtp_writer *)base->impl : NULL;

    if (!w || !unit || !unit->body.data || unit->body.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (unit->kind != ZM_RAW_RTP) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return w->sink.write(w->sink.ctx, unit->body.data, unit->body.len) < 0 ? ZM_ERR_IO : ZM_OK;
}

static int rtp_writer_flush(zm_writer *base, int force_boundary)
{
    (void)base;
    (void)force_boundary;
    return ZM_OK;
}

static int rtp_writer_finish(zm_writer *base)
{
    (void)base;
    return ZM_OK;
}

static void rtp_writer_close(zm_writer *base)
{
    zm_rtp_writer *w = base ? (zm_rtp_writer *)base->impl : NULL;

    if (!w) {
        return;
    }
    if (w->encoder) {
        rtp_payload_encode_destroy(w->encoder);
    }
    free(w);
}

static const zm_writer_ops k_rtp_writer_ops = {
    .write = rtp_writer_write,
    .write_raw = rtp_writer_write_raw,
    .flush = rtp_writer_flush,
    .finish = rtp_writer_finish,
    .close = rtp_writer_close,
};

zm_writer *zm_rtp_writer_open(const zm_sink *sink, const zm_rtp_writer_cfg *cfg)
{
    zm_rtp_writer *w;
    const zm_codec_desc *desc;
    zm_codec_id codec;
    const char *encoding;
    int payload_type;
    int clock_hz;
    struct rtp_payload_t handler;

    if (!sink || !sink->write || !cfg) {
        return NULL;
    }
    codec = cfg->codec;
    if (codec == ZM_CODEC_INVALID) {
        codec = zm_codec_from_rtpmap(cfg->encoding, cfg->clock_hz);
    }
    desc = zm_codec_get(codec);
    if (!desc) {
        return NULL;
    }
    encoding = cfg->encoding ? cfg->encoding : desc->map.rtp_encoding;
    if (!encoding) {
        return NULL;
    }
    payload_type = cfg->payload_type > 0 ? cfg->payload_type : desc->map.rtp_payload;
    clock_hz = cfg->clock_hz > 0 ? cfg->clock_hz : desc->clock_hz;

    w = (zm_rtp_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->sink = *sink;
    w->codec = codec;
    w->clock_hz = clock_hz;
    if (cfg->max_packet_bytes > 0) {
        rtp_packet_setsize(cfg->max_packet_bytes);
    }
    handler.alloc = rtp_writer_alloc;
    handler.free = rtp_writer_free;
    handler.packet = rtp_writer_packet;
    w->encoder = rtp_payload_encode_create(payload_type, encoding, cfg->seq, cfg->ssrc, &handler, w);
    if (!w->encoder) {
        free(w);
        return NULL;
    }
    w->base.ops = &k_rtp_writer_ops;
    w->base.impl = w;
    return &w->base;
}

int zm_rtp_writer_write_ts(zm_writer *base, const void *data, size_t len, uint32_t rtp_ts)
{
    zm_rtp_writer *w = base ? (zm_rtp_writer *)base->impl : NULL;

    if (!w || !w->encoder || !data || len == 0) {
        return ZM_ERR_INVALID;
    }
    return rtp_payload_encode_input(w->encoder, data, (int)len, rtp_ts) >= 0 ? ZM_OK
                                                                            : ZM_ERR_INVALID;
}
