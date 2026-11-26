#include "zm/transport/rtp.h"

#include "zm/core/codec.h"

#include "rtp-demuxer.h"
#include "rtp-payload.h"

#include <stdlib.h>
#include <string.h>

#define ZM_RTP_QUEUE_MAX 64

typedef struct zm_rtp_reader {
    zm_reader base;
    struct rtp_demuxer_t *demux;
    zm_codec_id codec;
    int clock_hz;
    uint32_t first_ts;
    int first_ts_set;
    zm_packet queue[ZM_RTP_QUEUE_MAX];
    unsigned head;
    unsigned tail;
    unsigned count;
    zm_track track;
    size_t track_count;
    uint8_t *pending;
    size_t pending_len;
    size_t pending_cap;
    int pending_active;
    uint32_t pending_ts;
    int pending_key;
} zm_rtp_reader;

static int rtp_queue_push(zm_rtp_reader *r, zm_packet *pkt)
{
    if (r->count == ZM_RTP_QUEUE_MAX) {
        zm_packet_clear(pkt);
        return ZM_ERR_BUFFER_TOO_SMALL;
    }
    r->queue[r->tail] = *pkt;
    r->tail = (r->tail + 1) % ZM_RTP_QUEUE_MAX;
    r->count++;
    zm_packet_init(pkt);
    return ZM_OK;
}

static int64_t rtp_ts_to_ms(zm_rtp_reader *r, uint32_t ts)
{
    int32_t delta;

    if (r->clock_hz <= 0) {
        return 0;
    }
    if (!r->first_ts_set) {
        r->first_ts = ts;
        r->first_ts_set = 1;
    }
    delta = (int32_t)(ts - r->first_ts);
    if (delta < 0) {
        delta = 0;
    }
    return (int64_t)((int64_t)delta * 1000 / r->clock_hz);
}

static int rtp_h264_is_key(const uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i + 4 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if ((d[i + 3] & 0x1f) == 5) {
                return 1;
            }
            i += 3;
        } else if (i + 5 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            if ((d[i + 4] & 0x1f) == 5) {
                return 1;
            }
            i += 4;
        } else {
            ++i;
        }
    }
    return 0;
}

static int rtp_h265_is_key(const uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i + 4 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int type = (d[i + 3] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) {
                return 1;
            }
            i += 3;
        } else if (i + 5 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            int type = (d[i + 4] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) {
                return 1;
            }
            i += 4;
        } else {
            ++i;
        }
    }
    return 0;
}


static void rtp_pending_reset(zm_rtp_reader *r)
{
    r->pending_len = 0;
    r->pending_active = 0;
    r->pending_key = 0;
}

static int rtp_pending_append(zm_rtp_reader *r, const uint8_t *data, size_t len)
{
    static const uint8_t sc[4] = {0, 0, 0, 1};
    size_t need = r->pending_len + len + 4;

    if (need > r->pending_cap) {
        size_t cap = r->pending_cap ? r->pending_cap * 2 : 4096;
        uint8_t *p;
        while (cap < need) {
            cap *= 2;
        }
        p = (uint8_t *)realloc(r->pending, cap);
        if (!p) {
            return -1;
        }
        r->pending = p;
        r->pending_cap = cap;
    }
    memcpy(r->pending + r->pending_len, sc, sizeof(sc));
    memcpy(r->pending + r->pending_len + sizeof(sc), data, len);
    r->pending_len += len + sizeof(sc);
    return 0;
}

static void rtp_pending_emit(zm_rtp_reader *r)
{
    zm_packet pkt;

    if (!r || !r->pending_active || r->pending_len == 0) {
        if (r) {
            rtp_pending_reset(r);
        }
        return;
    }
    zm_packet_init(&pkt);
    if (zm_mem_dup(r->pending, r->pending_len, &pkt.mem) != ZM_OK) {
        rtp_pending_reset(r);
        return;
    }
    pkt.codec = r->codec;
    pkt.kind = zm_codec_kind(r->codec);
    pkt.track_id = 0;
    pkt.dts_ms = rtp_ts_to_ms(r, r->pending_ts);
    pkt.pts_ms = pkt.dts_ms;
    if (r->pending_key) {
        pkt.flags |= ZM_PKT_KEY;
    }
    pkt.es_format = ZM_ES_ANNEXB;
    rtp_queue_push(r, &pkt);
    rtp_pending_reset(r);
}

static int rtp_nal_is_key(zm_codec_id codec, const uint8_t *nal, size_t len)
{
    if (!nal || len == 0) {
        return 0;
    }
    if (codec == ZM_CODEC_H264) {
        return (nal[0] & 0x1f) == 5;
    }
    if (codec == ZM_CODEC_H265) {
        int type = (nal[0] >> 1) & 0x3f;
        return type >= 16 && type <= 21;
    }
    if (codec == ZM_CODEC_H266) {
        int type = (nal[1] >> 3) & 0x1f;
        return type >= 16 && type <= 21;
    }
    return 0;
}

static int rtp_payload_is_key(zm_codec_id codec, const uint8_t *data, size_t len)
{
    if (codec == ZM_CODEC_H264) {
        return rtp_h264_is_key(data, len);
    }
    if (codec == ZM_CODEC_H265) {
        return rtp_h265_is_key(data, len);
    }
    return 0;
}

static int rtp_on_packet(void *param, const void *packet, int bytes, uint32_t timestamp, int flags)
{
    zm_rtp_reader *r = (zm_rtp_reader *)param;
    zm_packet pkt;
    zm_kind kind;

    if (!r || !packet || bytes <= 0) {
        return 0;
    }
    kind = zm_codec_kind(r->codec);

    /* H264/H265/H266：rtp_demuxer 按 NAL 回调，这里按 RTP timestamp 组装 AU。 */
    if (r->codec == ZM_CODEC_H264 || r->codec == ZM_CODEC_H265 || r->codec == ZM_CODEC_H266) {
        if (r->pending_active && timestamp != r->pending_ts) {
            rtp_pending_emit(r);
        }
        if (!r->pending_active) {
            r->pending_active = 1;
            r->pending_ts = timestamp;
            r->pending_key = 0;
            r->pending_len = 0;
        }
        if (rtp_pending_append(r, (const uint8_t *)packet, (size_t)bytes) != 0) {
            return 0;
        }
        if (rtp_nal_is_key(r->codec, (const uint8_t *)packet, (size_t)bytes)) {
            r->pending_key = 1;
        }
        return 0;
    }

    zm_packet_init(&pkt);
    if (zm_mem_dup(packet, (size_t)bytes, &pkt.mem) != ZM_OK) {
        return 0;
    }
    pkt.codec = r->codec;
    pkt.kind = kind;
    pkt.track_id = 0;
    pkt.dts_ms = rtp_ts_to_ms(r, timestamp);
    pkt.pts_ms = pkt.dts_ms;
    pkt.flags = 0;
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        pkt.flags |= ZM_PKT_LOST;
    }
    if (rtp_payload_is_key(r->codec, (const uint8_t *)packet, (size_t)bytes)) {
        pkt.flags |= ZM_PKT_KEY;
    }
    pkt.es_format = kind == ZM_KIND_VIDEO ? ZM_ES_ANNEXB : ZM_ES_RAW;
    rtp_queue_push(r, &pkt);
    return 0;
}

static int rtp_reader_input(zm_reader *base, const void *buf, size_t len)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r || !r->demux || !buf || len == 0) {
        return ZM_ERR_INVALID;
    }
    return rtp_demuxer_input(r->demux, buf, (int)len) >= 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int rtp_reader_read(zm_reader *base, zm_packet *out)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r || !out) {
        return ZM_ERR_INVALID;
    }
    if (r->count == 0) {
        return 0;
    }
    *out = r->queue[r->head];
    zm_packet_init(&r->queue[r->head]);
    r->head = (r->head + 1) % ZM_RTP_QUEUE_MAX;
    r->count--;
    return 1;
}

static int rtp_reader_read_raw(zm_reader *base, zm_raw_unit *out)
{
    (void)base;
    (void)out;
    return ZM_ERR_NOT_SUPPORTED;
}

static int rtp_reader_seek(zm_reader *base, int64_t *ms)
{
    (void)base;
    (void)ms;
    return ZM_ERR_NOT_SUPPORTED;
}

static const zm_track *rtp_reader_tracks(zm_reader *base, size_t *count)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    if (count) {
        *count = r->track_count;
    }
    return &r->track;
}

static void rtp_reader_close(zm_reader *base)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r) {
        return;
    }
    if (r->demux) {
        rtp_demuxer_destroy(&r->demux);
    }
    rtp_pending_reset(r);
    free(r->pending);
    r->pending = NULL;
    r->pending_cap = 0;
    while (r->count) {
        zm_packet_clear(&r->queue[r->head]);
        r->head = (r->head + 1) % ZM_RTP_QUEUE_MAX;
        r->count--;
    }
    zm_track_clear(&r->track);
    free(r);
}

static const zm_reader_ops k_rtp_reader_ops = {
    .input = rtp_reader_input,
    .read = rtp_reader_read,
    .read_raw = rtp_reader_read_raw,
    .seek = rtp_reader_seek,
    .tracks = rtp_reader_tracks,
    .close = rtp_reader_close,
};

zm_reader *zm_rtp_reader_open(const zm_rtp_reader_cfg *cfg)
{
    zm_rtp_reader *r;
    const zm_codec_desc *desc;
    zm_codec_id codec;
    const char *encoding;
    int payload_type;
    int clock_hz;

    if (!cfg) {
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

    r = (zm_rtp_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->base.ops = &k_rtp_reader_ops;
    r->base.impl = r;
    r->codec = codec;
    r->clock_hz = clock_hz;
    r->track_count = 1;
    zm_track_init(&r->track);
    r->track.track_id = 0;
    r->track.codec = codec;
    r->track.kind = desc->kind;
    r->track.clock_hz = clock_hz;
    r->track.timebase.num = 1;
    r->track.timebase.den = 1000;
    if (desc->kind == ZM_KIND_AUDIO) {
        r->track.sample_rate = clock_hz;
    }

    r->demux = rtp_demuxer_create(cfg->jitter_ms > 0 ? cfg->jitter_ms : 0, clock_hz, payload_type,
                                  encoding, rtp_on_packet, r);
    if (!r->demux) {
        zm_track_clear(&r->track);
        free(r);
        return NULL;
    }
    return &r->base;
}

int zm_rtp_reader_input_rtp(zm_reader *base, const void *pkt, size_t len)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r || !r->demux || !pkt || len == 0) {
        return ZM_ERR_INVALID;
    }
    return rtp_demuxer_input(r->demux, pkt, (int)len) >= 0 ? ZM_OK : ZM_ERR_INVALID;
}

int zm_rtp_reader_rtcp_report(zm_reader *base, void *buf, int len)
{
    zm_rtp_reader *r = base ? (zm_rtp_reader *)base->impl : NULL;

    if (!r || !r->demux || !buf || len <= 0) {
        return ZM_ERR_INVALID;
    }
    return rtp_demuxer_rtcp(r->demux, buf, len);
}
