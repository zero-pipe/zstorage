#include "zm/format/flv.h"

#include "zm/core/codec.h"
#include "zm/core/io.h"

#include "flv-demuxer.h"
#include "flv-header.h"
#include "flv-proto.h"
#include "flv-reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZM_FLV_QUEUE_MAX 64
#define ZM_FLV_TAG_MIN (64 * 1024)

typedef struct zm_flv_reader {
    zm_reader base;
    flv_demuxer_t *demuxer;
    zm_packet queue[ZM_FLV_QUEUE_MAX];
    unsigned head;
    unsigned tail;
    unsigned count;
    zm_track tracks[2]; /* 0=video, 1=audio */
    size_t track_count;
    int has_video;
    int has_audio;

    /* 文件（pull）模式 */
    int file_mode;
    void *file; /* flv_reader_t* */
    zm_io io;
    uint8_t *tagbuf;
    size_t tagcap;
    int eof;
    int suppress_all;    /* probe：更新 track/config，不产出包 */
    int suppress_config; /* 文件模式：config 只走 track，不产出 config 包 */
    int seek_scan;       /* seek 扫描中：抑制扫描阶段的媒体包 */
    int64_t seek_target_ms;
    int probe_video_media; /* probe 阶段是否已见到视频媒体帧 */
} zm_flv_reader;

static const int k_aac_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

static zm_track *flv_reader_track(zm_flv_reader *r, zm_kind kind)
{
    if (kind == ZM_KIND_VIDEO) {
        return &r->tracks[0];
    }
    if (kind == ZM_KIND_AUDIO) {
        return &r->tracks[1];
    }
    return NULL;
}

static void flv_reader_update_track(zm_flv_reader *r, zm_codec_id codec, zm_kind kind,
                                    int sample_rate, int channels)
{
    zm_track *t = flv_reader_track(r, kind);
    if (!t) {
        return;
    }
    if (t->codec == ZM_CODEC_INVALID) {
        t->track_id = kind == ZM_KIND_VIDEO ? 0u : 1u;
        t->codec = codec;
        t->kind = kind;
        t->timebase.num = 1;
        t->timebase.den = 1000;
        if (kind == ZM_KIND_VIDEO) {
            t->clock_hz = 90000;
            r->has_video = 1;
        } else {
            t->clock_hz = sample_rate > 0 ? sample_rate : zm_codec_clock_hz(codec);
            t->sample_rate = sample_rate;
            t->channels = channels;
            r->has_audio = 1;
        }
    } else if (kind == ZM_KIND_AUDIO) {
        if (sample_rate > 0) {
            t->clock_hz = sample_rate;
            t->sample_rate = sample_rate;
        }
        if (channels > 0) {
            t->channels = channels;
        }
    }
}

static void flv_aac_asc_info(const uint8_t *asc, size_t len, int *rate, int *channels)
{
    int idx;
    int ch;

    if (!asc || len < 2) {
        return;
    }
    idx = ((asc[0] & 0x07) << 1) | ((asc[1] >> 7) & 0x01);
    ch = (asc[1] >> 3) & 0x0f;
    if (idx >= 0 && idx < (int)(sizeof(k_aac_rates) / sizeof(k_aac_rates[0]))) {
        *rate = k_aac_rates[idx];
    }
    if (ch > 0 && ch <= 7) {
        *channels = ch;
    }
}

static size_t flv_adts_header_len(const uint8_t *p, size_t len)
{
    if (len < 7 || p[0] != 0xff || (p[1] & 0xf0) != 0xf0) {
        return 0;
    }
    return (p[1] & 0x01) ? 7u : 9u;
}

static int flv_config_codec(int codec, zm_codec_id *id, zm_kind *kind, zm_config_fmt *fmt)
{
    switch (codec) {
    case FLV_VIDEO_AVCC:
        *id = ZM_CODEC_H264;
        *kind = ZM_KIND_VIDEO;
        *fmt = ZM_CONFIG_AVCC;
        return 1;
    case FLV_VIDEO_HVCC:
        *id = ZM_CODEC_H265;
        *kind = ZM_KIND_VIDEO;
        *fmt = ZM_CONFIG_HVCC;
        return 1;
    case FLV_VIDEO_VVCC:
        *id = ZM_CODEC_H266;
        *kind = ZM_KIND_VIDEO;
        *fmt = ZM_CONFIG_VVCC;
        return 1;
    case FLV_VIDEO_AV1C:
        *id = ZM_CODEC_AV1;
        *kind = ZM_KIND_VIDEO;
        *fmt = ZM_CONFIG_AV1C;
        return 1;
    case FLV_AUDIO_ASC:
        *id = ZM_CODEC_AAC;
        *kind = ZM_KIND_AUDIO;
        *fmt = ZM_CONFIG_ASC;
        return 1;
    case FLV_AUDIO_OPUS_HEAD:
        *id = ZM_CODEC_OPUS;
        *kind = ZM_KIND_AUDIO;
        *fmt = ZM_CONFIG_OPUS_HEAD;
        return 1;
    default:
        return 0;
    }
}

static int flv_queue_push(zm_flv_reader *r, zm_packet *pkt)
{
    if (r->count == ZM_FLV_QUEUE_MAX) {
        zm_packet_clear(pkt);
        return ZM_ERR_BUFFER_TOO_SMALL;
    }
    r->queue[r->tail] = *pkt;
    r->tail = (r->tail + 1) % ZM_FLV_QUEUE_MAX;
    r->count++;
    zm_packet_init(pkt);
    return ZM_OK;
}

static void flv_queue_drain(zm_flv_reader *r)
{
    while (r->count) {
        zm_packet_clear(&r->queue[r->head]);
        r->head = (r->head + 1) % ZM_FLV_QUEUE_MAX;
        r->count--;
    }
}

static int flv_on_es(void *param, int codec, const void *data, size_t bytes, uint32_t pts,
                     uint32_t dts, int flags)
{
    zm_flv_reader *r = (zm_flv_reader *)param;
    zm_codec_id id;
    zm_kind kind;
    zm_config_fmt fmt;
    zm_packet pkt;
    zm_track *track;
    const uint8_t *payload = (const uint8_t *)data;
    size_t len = bytes;
    uint8_t es_format;

    if (!r || !data || bytes == 0) {
        return 0;
    }

    if (flv_config_codec(codec, &id, &kind, &fmt)) {
        int rate = 0;
        int channels = 0;
        if (id == ZM_CODEC_AAC) {
            flv_aac_asc_info(payload, len, &rate, &channels);
        } else if (id == ZM_CODEC_OPUS) {
            rate = 48000;
            channels = 2;
        }
        flv_reader_update_track(r, id, kind, rate, channels);
        track = flv_reader_track(r, kind);
        if (track) {
            zm_track_set_config(track, fmt, payload, len);
        }
        if (r->suppress_all || r->suppress_config) {
            return 0;
        }
        zm_packet_init(&pkt);
        if (zm_mem_dup(payload, len, &pkt.mem) != ZM_OK) {
            return 0;
        }
        pkt.codec = id;
        pkt.kind = kind;
        pkt.track_id = kind == ZM_KIND_VIDEO ? 0u : 1u;
        pkt.dts_ms = (int64_t)dts;
        pkt.pts_ms = (int64_t)pts;
        pkt.flags = ZM_PKT_CONFIG;
        pkt.es_format = kind == ZM_KIND_VIDEO ? ZM_ES_ANNEXB : ZM_ES_RAW;
        flv_queue_push(r, &pkt);
        return 0;
    }

    if (codec == FLV_SCRIPT_METADATA) {
        return 0;
    }

    id = zm_codec_from_flv(codec);
    if (id == ZM_CODEC_INVALID) {
        return 0;
    }
    kind = zm_codec_kind(id);
    if (kind == ZM_KIND_UNKNOWN) {
        return 0;
    }

    if (id == ZM_CODEC_AAC) {
        size_t hdr = flv_adts_header_len(payload, len);
        if (hdr > 0) {
            payload += hdr;
            len -= hdr;
        }
    }

    flv_reader_update_track(r, id, kind, 0, 0);
    if (kind == ZM_KIND_VIDEO) {
        r->probe_video_media = 1;
    }

    if (r->suppress_all || r->seek_scan) {
        return 0;
    }

    zm_packet_init(&pkt);
    if (len == 0 || zm_mem_dup(payload, len, &pkt.mem) != ZM_OK) {
        return 0;
    }
    pkt.codec = id;
    pkt.kind = kind;
    pkt.track_id = kind == ZM_KIND_VIDEO ? 0u : 1u;
    pkt.dts_ms = (int64_t)dts;
    pkt.pts_ms = (int64_t)pts;
    pkt.flags = flags ? ZM_PKT_KEY : 0;
    es_format = kind == ZM_KIND_VIDEO ? ZM_ES_ANNEXB : ZM_ES_RAW;
    pkt.es_format = es_format;
    flv_queue_push(r, &pkt);
    return 0;
}

/* ---- 文件（pull）模式 ---- */

static int flv_file_read_cb(void *param, void *buf, int len)
{
    zm_flv_reader *r = (zm_flv_reader *)param;

    if (!r || !r->io.read) {
        return -1;
    }
    return r->io.read(r->io.ctx, buf, (size_t)len);
}

static int flv_video_is_key(const uint8_t *body, size_t len)
{
    struct flv_video_tag_header_t vh;

    memset(&vh, 0, sizeof(vh));
    if (flv_video_tag_header_read(&vh, body, len) < 1) {
        return 0;
    }
    return vh.keyframe == FLV_VIDEO_KEY_FRAME;
}

/** 读一个 FLV tag body；1-成功，0-EOF，<0-错误。 */
static int flv_read_one_tag(zm_flv_reader *r, int *type, uint32_t *ts, const uint8_t **body,
                            size_t *len)
{
    int tagtype = 0;
    uint32_t timestamp = 0;
    size_t taglen = 0;
    int n;

    if (!r || !r->file || !type || !ts || !body || !len) {
        return 0;
    }
    if (r->tagcap < ZM_FLV_TAG_MIN) {
        uint8_t *p = (uint8_t *)realloc(r->tagbuf, ZM_FLV_TAG_MIN);
        if (!p) {
            return -1;
        }
        r->tagbuf = p;
        r->tagcap = ZM_FLV_TAG_MIN;
    }
    n = flv_reader_read(r->file, &tagtype, &timestamp, &taglen, r->tagbuf, r->tagcap);
    if (n == FLV_READER_NEED_BUF) {
        uint8_t *p;
        if (taglen == 0) {
            return -1;
        }
        p = (uint8_t *)realloc(r->tagbuf, taglen);
        if (!p) {
            return -1;
        }
        r->tagbuf = p;
        r->tagcap = taglen;
        n = flv_reader_read(r->file, &tagtype, &timestamp, &taglen, r->tagbuf, r->tagcap);
    }
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        r->eof = 1;
        return 0;
    }
    if (taglen == 0) {
        return 1;
    }
    *type = tagtype;
    *ts = timestamp;
    *body = r->tagbuf;
    *len = taglen;
    return 1;
}

static int flv_file_reopen(zm_flv_reader *r)
{
    if (r->file) {
        flv_reader_destroy(r->file);
        r->file = NULL;
    }
    if (r->demuxer) {
        flv_demuxer_destroy(r->demuxer);
        r->demuxer = NULL;
    }
    flv_queue_drain(r);
    r->eof = 0;
    if (r->io.seek) {
        (void)r->io.seek(r->io.ctx, 0, SEEK_SET);
    }
    r->file = flv_reader_create2(flv_file_read_cb, r);
    r->demuxer = flv_demuxer_create(flv_on_es, r);
    if (!r->file || !r->demuxer) {
        return -1;
    }
    return 0;
}

static int flv_reader_input(zm_reader *base, const void *buf, size_t len)
{
    (void)base;
    (void)buf;
    (void)len;
    return ZM_ERR_NOT_SUPPORTED;
}

static int flv_reader_read_pkt(zm_reader *base, zm_packet *out)
{
    zm_flv_reader *r = base ? (zm_flv_reader *)base->impl : NULL;

    if (!r || !out) {
        return ZM_ERR_INVALID;
    }
    if (!r->file_mode) {
        if (r->count == 0) {
            return 0;
        }
    } else {
        for (;;) {
            int type = 0;
            uint32_t ts = 0;
            const uint8_t *body = NULL;
            size_t len = 0;
            int n;

            if (r->count > 0) {
                break;
            }
            if (r->eof) {
                return 0;
            }
            n = flv_read_one_tag(r, &type, &ts, &body, &len);
            if (n < 0) {
                return ZM_ERR_IO;
            }
            if (n == 0) {
                return 0;
            }
            if (len == 0) {
                continue;
            }
            (void)flv_demuxer_input(r->demuxer, type, body, (int)len, ts);
        }
    }
    *out = r->queue[r->head];
    zm_packet_init(&r->queue[r->head]);
    r->head = (r->head + 1) % ZM_FLV_QUEUE_MAX;
    r->count--;
    return 1;
}

static int flv_reader_read_raw(zm_reader *base, zm_raw_unit *out)
{
    (void)base;
    (void)out;
    return ZM_ERR_NOT_SUPPORTED;
}

static int flv_reader_seek(zm_reader *base, int64_t *ms)
{
    zm_flv_reader *r = base ? (zm_flv_reader *)base->impl : NULL;
    int64_t target;

    if (!r || !ms) {
        return ZM_ERR_INVALID;
    }
    if (!r->file_mode) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    target = *ms < 0 ? 0 : *ms;
    r->seek_target_ms = target;
    r->seek_scan = 1;
    if (flv_file_reopen(r) != 0) {
        r->seek_scan = 0;
        return ZM_ERR_IO;
    }

    for (;;) {
        int type = 0;
        uint32_t ts = 0;
        const uint8_t *body = NULL;
        size_t len = 0;
        int n;

        if (r->eof) {
            break;
        }
        n = flv_read_one_tag(r, &type, &ts, &body, &len);
        if (n < 0) {
            r->seek_scan = 0;
            return ZM_ERR_IO;
        }
        if (n == 0) {
            break;
        }
        if (len == 0) {
            continue;
        }
        if (type == FLV_TYPE_VIDEO && (int64_t)ts >= target && flv_video_is_key(body, len)) {
            r->seek_scan = 0; /* 关键帧：正常喂入，媒体包进队列 */
            (void)flv_demuxer_input(r->demuxer, type, body, (int)len, ts);
            *ms = (int64_t)ts;
            return ZM_OK;
        }
        if (type == FLV_TYPE_AUDIO && r->tracks[0].codec == ZM_CODEC_INVALID &&
            (int64_t)ts >= target) {
            r->seek_scan = 0;
            (void)flv_demuxer_input(r->demuxer, type, body, (int)len, ts);
            *ms = (int64_t)ts;
            return ZM_OK;
        }
        /* 扫描阶段只更新 track/config，不产出媒体包 */
        (void)flv_demuxer_input(r->demuxer, type, body, (int)len, ts);
    }

    /* 未找到同步点：回到文件开头 */
    r->seek_scan = 0;
    *ms = 0;
    if (flv_file_reopen(r) != 0) {
        return ZM_ERR_IO;
    }
    return ZM_OK;
}

static const zm_track *flv_reader_tracks(zm_reader *base, size_t *count)
{
    zm_flv_reader *r = base ? (zm_flv_reader *)base->impl : NULL;

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

static void flv_reader_close(zm_reader *base)
{
    zm_flv_reader *r = base ? (zm_flv_reader *)base->impl : NULL;

    if (!r) {
        return;
    }
    if (r->file) {
        flv_reader_destroy(r->file);
    }
    if (r->demuxer) {
        flv_demuxer_destroy(r->demuxer);
    }
    flv_queue_drain(r);
    zm_track_clear(&r->tracks[0]);
    zm_track_clear(&r->tracks[1]);
    free(r->tagbuf);
    free(r);
}

static const zm_reader_ops k_flv_reader_ops = {
    .input = flv_reader_input,
    .read = flv_reader_read_pkt,
    .read_raw = flv_reader_read_raw,
    .seek = flv_reader_seek,
    .tracks = flv_reader_tracks,
    .close = flv_reader_close,
};

static zm_flv_reader *flv_reader_alloc(void)
{
    zm_flv_reader *r;

    r = (zm_flv_reader *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->base.ops = &k_flv_reader_ops;
    r->base.impl = r;
    r->track_count = 2;
    zm_track_init(&r->tracks[0]);
    zm_track_init(&r->tracks[1]);
    r->demuxer = flv_demuxer_create(flv_on_es, r);
    if (!r->demuxer) {
        free(r);
        return NULL;
    }
    return r;
}

zm_reader *zm_flv_reader_open_push(const zm_reader_cfg *cfg)
{
    zm_flv_reader *r;

    if (cfg && (cfg->mode & ZM_READ_RAW)) {
        return NULL;
    }
    r = flv_reader_alloc();
    return r ? &r->base : NULL;
}

zm_reader *zm_flv_reader_open_file(const zm_io *io, const zm_reader_cfg *cfg)
{
    zm_flv_reader *r;

    if (!io || !io->read || !io->seek || !io->tell) {
        return NULL;
    }
    if (cfg && (cfg->mode & ZM_READ_RAW)) {
        return NULL;
    }
    r = flv_reader_alloc();
    if (!r) {
        return NULL;
    }
    r->file_mode = 1;
    r->suppress_config = 1;
    r->io = *io;
    if (r->io.seek) {
        (void)r->io.seek(r->io.ctx, 0, SEEK_SET);
    }
    r->file = flv_reader_create2(flv_file_read_cb, r);
    if (!r->file) {
        flv_reader_close(&r->base);
        return NULL;
    }

    /* 探测：读到视频 config，填充 tracks，再从头重开。 */
    r->probe_video_media = 0;
    r->suppress_all = 1;
    for (;;) {
        int type = 0;
        uint32_t ts = 0;
        const uint8_t *body = NULL;
        size_t len = 0;
        int n = flv_read_one_tag(r, &type, &ts, &body, &len);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        if (len > 0) {
            (void)flv_demuxer_input(r->demuxer, type, body, (int)len, ts);
        }
        /* 视频 codec 必有；再拿到音频 codec 或见到首个视频媒体帧即止。 */
        if (r->tracks[0].codec != ZM_CODEC_INVALID &&
            (r->tracks[1].codec != ZM_CODEC_INVALID || r->probe_video_media)) {
            break;
        }
    }
    r->suppress_all = 0;
    if (flv_file_reopen(r) != 0) {
        flv_reader_close(&r->base);
        return NULL;
    }
    return &r->base;
}

int zm_flv_reader_input_tag(zm_reader *base, int type, const void *body, size_t len,
                             uint32_t dts_ms)
{
    zm_flv_reader *r = base ? (zm_flv_reader *)base->impl : NULL;

    if (!r || !r->demuxer || !body || len == 0) {
        return ZM_ERR_INVALID;
    }
    return flv_demuxer_input(r->demuxer, type, body, len, dts_ms) >= 0 ? ZM_OK : ZM_ERR_INVALID;
}
