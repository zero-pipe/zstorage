#include "zm/format/mpeg.h"

#include "zm/core/codec.h"

#include "mpeg-ps.h"
#include "mpeg-ts.h"

#include <stdlib.h>
#include <string.h>


typedef struct zm_mpeg_reader {
    zm_reader base;
    int is_ps;
    struct ts_demuxer_t *ts;
    struct ps_demuxer_t *ps;
    uint8_t ts_pending[188];
    size_t ts_pending_len;
    uint8_t *ps_buf;
    size_t ps_len;
    size_t ps_cap;
    zm_packet *queue;
    size_t head;
    size_t count;
    size_t cap;
    zm_track tracks[2];
    size_t track_count;
} zm_mpeg_reader;

static int mpeg_queue_push(zm_mpeg_reader *r, zm_packet *pkt)
{
    if (r->count == r->cap) {
        if (r->head > 0) {
            memmove(r->queue, r->queue + r->head, r->count * sizeof(*r->queue));
            r->head = 0;
        }
        if (r->count == r->cap) {
            size_t cap = r->cap ? r->cap * 2 : 64;
            zm_packet *q = (zm_packet *)realloc(r->queue, cap * sizeof(*q));
            if (!q) {
                zm_packet_clear(pkt);
                return ZM_ERR_NOMEM;
            }
            r->queue = q;
            r->cap = cap;
        }
    }
    r->queue[r->head + r->count] = *pkt;
    r->count++;
    zm_packet_init(pkt);
    return ZM_OK;
}

static const int k_aac_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

typedef struct mpeg_adts_info {
    size_t header;
    size_t frame;
    int rate;
    int channels;
} mpeg_adts_info;

static int mpeg_adts_parse(const uint8_t *p, size_t len, mpeg_adts_info *info)
{
    int idx;
    int ch;

    if (!p || !info || len < 7 || p[0] != 0xff || (p[1] & 0xf0) != 0xf0) {
        return 0;
    }
    info->header = (p[1] & 0x01) ? 7u : 9u;
    info->frame = ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) |
                  ((size_t)(p[5] & 0xe0) >> 5);
    idx = (p[2] >> 2) & 0x0f;
    ch = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
    info->rate = (idx >= 0 && idx < 13) ? k_aac_rates[idx] : 0;
    info->channels = (ch > 0 && ch <= 7) ? ch : 0;
    return info->frame >= info->header && info->frame <= len;
}

static zm_track *mpeg_reader_track(zm_mpeg_reader *r, zm_kind kind)
{
    if (kind == ZM_KIND_VIDEO) {
        return &r->tracks[0];
    }
    if (kind == ZM_KIND_AUDIO) {
        return &r->tracks[1];
    }
    return NULL;
}

static void mpeg_reader_update_track(zm_mpeg_reader *r, zm_codec_id codec, zm_kind kind)
{
    zm_track *t = mpeg_reader_track(r, kind);

    if (!t || t->codec != ZM_CODEC_INVALID) {
        return;
    }
    t->track_id = kind == ZM_KIND_VIDEO ? 0u : 1u;
    t->codec = codec;
    t->kind = kind;
    t->timebase.num = 1;
    t->timebase.den = 1000;
    if (kind == ZM_KIND_VIDEO) {
        t->clock_hz = 90000;
    } else {
        t->clock_hz = zm_codec_clock_hz(codec);
        t->sample_rate = t->clock_hz;
    }
}

static int mpeg_emit_one(zm_mpeg_reader *r, zm_codec_id codec, zm_kind kind, int flags,
                         int64_t pts, int64_t dts, const uint8_t *data, size_t len)
{
    zm_packet pkt;

    if (!r || !data || len == 0) {
        return 0;
    }
    zm_packet_init(&pkt);
    if (zm_mem_dup(data, len, &pkt.mem) != ZM_OK) {
        return 0;
    }
    pkt.codec = codec;
    pkt.kind = kind;
    pkt.track_id = kind == ZM_KIND_VIDEO ? 0u : 1u;
    pkt.dts_ms = dts;
    pkt.pts_ms = pts;
    pkt.flags = 0;
    if (flags & MPEG_FLAG_IDR_FRAME) {
        pkt.flags |= ZM_PKT_KEY;
    }
    if (flags & (MPEG_FLAG_PACKET_LOST | MPEG_FLAG_PACKET_CORRUPT)) {
        pkt.flags |= ZM_PKT_LOST;
    }
    pkt.es_format = kind == ZM_KIND_VIDEO ? ZM_ES_ANNEXB : ZM_ES_RAW;
    mpeg_queue_push(r, &pkt);
    return 0;
}

static int mpeg_emit(zm_mpeg_reader *r, int codecid, int flags, int64_t pts_90k, int64_t dts_90k,
                     const void *data, size_t bytes)
{
    zm_codec_id codec;
    zm_kind kind;
    const uint8_t *payload = (const uint8_t *)data;
    size_t len = bytes;
    int64_t pts = pts_90k / 90;
    int64_t dts = dts_90k / 90;

    if (!r || !data || bytes == 0) {
        return 0;
    }
    codec = zm_codec_from_psi(codecid);
    if (codec == ZM_CODEC_INVALID) {
        return 0;
    }
    kind = zm_codec_kind(codec);
    if (kind == ZM_KIND_UNKNOWN) {
        return 0;
    }
    mpeg_reader_update_track(r, codec, kind);

    if (codec != ZM_CODEC_AAC) {
        return mpeg_emit_one(r, codec, kind, flags, pts, dts, payload, len);
    }

    /* 一个 PES 可能包含多个 ADTS 帧；规范模型要求一个 packet 一个 AAC 帧。 */
    {
        size_t off = 0;
        int64_t frame_index = 0;
        int base_rate = 0;
        mpeg_adts_info info;

        while (off + 7 <= len && mpeg_adts_parse(payload + off, len - off, &info)) {
            int rate = info.rate > 0 ? info.rate : (base_rate > 0 ? base_rate : 44100);
            int64_t frame_ms = (int64_t)1024 * 1000 / rate;

            if (base_rate == 0) {
                zm_track *t = mpeg_reader_track(r, ZM_KIND_AUDIO);
                base_rate = rate;
                if (t) {
                    t->clock_hz = rate;
                    t->sample_rate = rate;
                    if (t->channels == 0 && info.channels > 0) {
                        t->channels = info.channels;
                    }
                }
            }
            mpeg_emit_one(r, codec, kind, 0, pts + frame_index * frame_ms,
                          dts + frame_index * frame_ms, payload + off + info.header,
                          info.frame - info.header);
            off += info.frame;
            ++frame_index;
        }
        if (off == 0) {
            return mpeg_emit_one(r, codec, kind, flags, pts, dts, payload, len);
        }
    }
    return 0;
}

static int mpeg_on_ts(void *param, int program, int stream, int codecid, int flags, int64_t pts,
                      int64_t dts, const void *data, size_t bytes)
{
    (void)program;
    (void)stream;
    return mpeg_emit((zm_mpeg_reader *)param, codecid, flags, pts, dts, data, bytes);
}

static int mpeg_on_ps(void *param, int stream, int codecid, int flags, int64_t pts, int64_t dts,
                      const void *data, size_t bytes)
{
    (void)stream;
    return mpeg_emit((zm_mpeg_reader *)param, codecid, flags, pts, dts, data, bytes);
}

static int mpeg_reader_input_ts(zm_mpeg_reader *r, const uint8_t *p, size_t len)
{
    if (r->ts_pending_len) {
        size_t need = sizeof(r->ts_pending) - r->ts_pending_len;
        size_t take = len < need ? len : need;
        memcpy(r->ts_pending + r->ts_pending_len, p, take);
        r->ts_pending_len += take;
        p += take;
        len -= take;
        if (r->ts_pending_len < sizeof(r->ts_pending)) {
            return ZM_OK;
        }
        if (ts_demuxer_input(r->ts, r->ts_pending, sizeof(r->ts_pending)) < 0) {
            return ZM_ERR_INVALID;
        }
        r->ts_pending_len = 0;
    }
    while (len >= sizeof(r->ts_pending)) {
        if (ts_demuxer_input(r->ts, p, sizeof(r->ts_pending)) < 0) {
            return ZM_ERR_INVALID;
        }
        p += sizeof(r->ts_pending);
        len -= sizeof(r->ts_pending);
    }
    if (len) {
        memcpy(r->ts_pending, p, len);
        r->ts_pending_len = len;
    }
    return ZM_OK;
}

static int mpeg_reader_input_ps(zm_mpeg_reader *r, const uint8_t *p, size_t len)
{
    size_t need = r->ps_len + len;
    size_t off = 0;

    if (need > r->ps_cap) {
        size_t cap = r->ps_cap ? r->ps_cap : 4096;
        uint8_t *q;
        while (cap < need) {
            cap *= 2;
        }
        q = (uint8_t *)realloc(r->ps_buf, cap);
        if (!q) {
            return ZM_ERR_NOMEM;
        }
        r->ps_buf = q;
        r->ps_cap = cap;
    }
    memcpy(r->ps_buf + r->ps_len, p, len);
    r->ps_len += len;

    while (off < r->ps_len) {
        int n = ps_demuxer_input(r->ps, r->ps_buf + off, r->ps_len - off);
        if (n < 0) {
            return ZM_ERR_INVALID;
        }
        if (n == 0) {
            break;
        }
        off += (size_t)n;
    }
    if (off) {
        memmove(r->ps_buf, r->ps_buf + off, r->ps_len - off);
        r->ps_len -= off;
    }
    return ZM_OK;
}

static int mpeg_reader_input(zm_reader *base, const void *buf, size_t len)
{
    zm_mpeg_reader *r = base ? (zm_mpeg_reader *)base->impl : NULL;

    if (!r || !buf || len == 0) {
        return ZM_ERR_INVALID;
    }
    if (r->is_ps) {
        return r->ps ? mpeg_reader_input_ps(r, (const uint8_t *)buf, len) : ZM_ERR_INVALID;
    }
    return r->ts ? mpeg_reader_input_ts(r, (const uint8_t *)buf, len) : ZM_ERR_INVALID;
}

static int mpeg_reader_read(zm_reader *base, zm_packet *out)
{
    zm_mpeg_reader *r = base ? (zm_mpeg_reader *)base->impl : NULL;

    if (!r || !out) {
        return ZM_ERR_INVALID;
    }
    if (r->count == 0) {
        return 0;
    }
    *out = r->queue[r->head];
    zm_packet_init(&r->queue[r->head]);
    r->head++;
    r->count--;
    if (r->count == 0) {
        r->head = 0;
    }
    return 1;
}

static int mpeg_reader_read_raw(zm_reader *base, zm_raw_unit *out)
{
    (void)base;
    (void)out;
    return ZM_ERR_NOT_SUPPORTED;
}

static int mpeg_reader_seek(zm_reader *base, int64_t *ms)
{
    (void)base;
    (void)ms;
    return ZM_ERR_NOT_SUPPORTED;
}

static const zm_track *mpeg_reader_tracks(zm_reader *base, size_t *count)
{
    zm_mpeg_reader *r = base ? (zm_mpeg_reader *)base->impl : NULL;

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

static void mpeg_reader_close(zm_reader *base)
{
    zm_mpeg_reader *r = base ? (zm_mpeg_reader *)base->impl : NULL;

    if (!r) {
        return;
    }
    if (r->ts) {
        ts_demuxer_destroy(r->ts);
    }
    if (r->ps) {
        ps_demuxer_destroy(r->ps);
    }
    while (r->count) {
        zm_packet_clear(&r->queue[r->head]);
        r->head++;
        r->count--;
    }
    zm_track_clear(&r->tracks[0]);
    zm_track_clear(&r->tracks[1]);
    free(r->ps_buf);
    free(r->queue);
    free(r);
}

static const zm_reader_ops k_mpeg_reader_ops = {
    .input = mpeg_reader_input,
    .read = mpeg_reader_read,
    .read_raw = mpeg_reader_read_raw,
    .seek = mpeg_reader_seek,
    .tracks = mpeg_reader_tracks,
    .close = mpeg_reader_close,
};

zm_reader *zm_mpeg_reader_open(const zm_reader_cfg *cfg)
{
    zm_mpeg_reader *r;

    if (!cfg || (cfg->format != ZM_FMT_MPEG_TS && cfg->format != ZM_FMT_MPEG_PS)) {
        return NULL;
    }
    r = (zm_mpeg_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->base.ops = &k_mpeg_reader_ops;
    r->base.impl = r;
    r->is_ps = cfg->format == ZM_FMT_MPEG_PS;
    r->track_count = 2;
    zm_track_init(&r->tracks[0]);
    zm_track_init(&r->tracks[1]);
    if (r->is_ps) {
        r->ps = ps_demuxer_create(mpeg_on_ps, r);
    } else {
        r->ts = ts_demuxer_create(mpeg_on_ts, r);
    }
    if (!r->ps && !r->ts) {
        free(r);
        return NULL;
    }
    return &r->base;
}
