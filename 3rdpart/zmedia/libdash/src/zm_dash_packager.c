#include "zm/packager/packager.h"

#include "zm/core/codec.h"

#include "dash-mpd.h"
#include "dash-proto.h"
#include "mov-format.h"

#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdlib.h>
#include <string.h>

#define ZM_DASH_MAX_TRACKS 8

typedef struct zm_dash_wtrack {
    zm_kind kind;
    zm_codec_id codec;
    int adapt;
    int codec_kind; /* 0-none, 1-avc, 2-hevc, 3-vvc */
    union {
        struct mpeg4_avc_t avc;
        struct mpeg4_hevc_t hevc;
        struct mpeg4_vvc_t vvc;
    } c;
} zm_dash_wtrack;

struct zm_dash_packager {
    dash_mpd_t *mpd;
    zm_packager_cb cb;
    void *ctx;
    int live;
    zm_dash_wtrack tracks[ZM_DASH_MAX_TRACKS];
    size_t track_count;
    uint8_t *conv;
    size_t conv_cap;
    int64_t last_pts;
    int64_t last_dts;
    int have_last;
};

static int dash_emit_playlist(struct zm_dash_packager *p)
{
    char text[16384];
    size_t n;

    if (!p || !p->mpd || !p->cb.on_playlist) {
        return 0;
    }
    n = dash_mpd_playlist(p->mpd, text, sizeof(text));
    if (n == 0 || n >= sizeof(text)) {
        return -1;
    }
    p->cb.on_playlist(p->ctx, "index.mpd", text, n);
    return 0;
}

static int dash_on_segment(void *param, int adaptation, const void *data, size_t bytes, int64_t pts,
                           int64_t dts, int64_t duration, const char *name)
{
    struct zm_dash_packager *p = (struct zm_dash_packager *)param;

    (void)adaptation;
    (void)dts;
    if (!p) {
        return 0;
    }
    if (p->cb.on_segment) {
        p->cb.on_segment(p->ctx, name ? name : "segment", data, bytes, pts, duration, 0);
    }
    dash_emit_playlist(p);
    return 0;
}

static int dash_find_track(struct zm_dash_packager *p, const zm_packet *pkt,
                           zm_dash_wtrack **out)
{
    size_t i;

    for (i = 0; i < p->track_count; ++i) {
        if (p->tracks[i].kind == pkt->kind) {
            *out = &p->tracks[i];
            return 0;
        }
    }
    return -1;
}

static int dash_ensure_conv(struct zm_dash_packager *p, size_t cap)
{
    if (cap <= p->conv_cap) {
        return 0;
    }
    {
        uint8_t *q = (uint8_t *)realloc(p->conv, cap);
        if (!q) {
            return -1;
        }
        p->conv = q;
        p->conv_cap = cap;
    }
    return 0;
}

static int dash_convert(struct zm_dash_packager *p, zm_dash_wtrack *t, const uint8_t *data,
                        size_t len, const uint8_t **out, size_t *out_len)
{
    size_t cap = len + 4096;
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        int n;
        int vcl = 0;
        int update = 0;
        if (dash_ensure_conv(p, cap) != 0) {
            return -1;
        }
        if (t->codec_kind == 1) {
            n = h264_annexbtomp4(&t->c.avc, data, len, p->conv, cap, &vcl, &update);
        } else if (t->codec_kind == 2) {
            n = h265_annexbtomp4(&t->c.hevc, data, len, p->conv, cap, &vcl, &update);
        } else {
            n = h266_annexbtomp4(&t->c.vvc, data, len, p->conv, cap, &vcl, &update);
        }
        if (n > 0) {
            *out = p->conv;
            *out_len = (size_t)n;
            return 0;
        }
        cap *= 2;
    }
    return -1;
}

static size_t dash_strip_adts(const uint8_t *data, size_t len)
{
    if (len >= 7 && data[0] == 0xff && (data[1] & 0xf0) == 0xf0) {
        return (data[1] & 0x01) ? 7u : 9u;
    }
    return 0;
}

static int dash_write(zm_packager *base, const zm_packet *pkt)
{
    struct zm_dash_packager *p = base ? (struct zm_dash_packager *)base->impl : NULL;
    zm_dash_wtrack *t;
    const uint8_t *data;
    size_t len;
    int flags = 0;
    int rc;

    if (!p || !p->mpd || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        return ZM_OK;
    }
    if (dash_find_track(p, pkt, &t) != 0) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    data = pkt->mem.data;
    len = pkt->mem.len;
    if (pkt->es_format == ZM_ES_ADTS) {
        size_t hdr = dash_strip_adts(data, len);
        data += hdr;
        len -= hdr;
    }
    if ((t->codec_kind == 1 || t->codec_kind == 2 || t->codec_kind == 3) &&
        pkt->es_format != ZM_ES_LENGTH_PREFIXED) {
        if (dash_convert(p, t, data, len, &data, &len) != 0) {
            return ZM_ERR_INVALID;
        }
    }
    if (pkt->flags & ZM_PKT_KEY) {
        flags |= MOV_AV_FLAG_KEYFREAME;
    }
    rc = dash_mpd_input(p->mpd, t->adapt, data, len, pkt->pts_ms, pkt->dts_ms, flags);
    if (rc == 0) {
        p->last_pts = pkt->pts_ms;
        p->last_dts = pkt->dts_ms;
        p->have_last = 1;
    }
    return rc == 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int dash_flush(zm_packager *base, int force)
{
    struct zm_dash_packager *p = base ? (struct zm_dash_packager *)base->impl : NULL;

    (void)force;
    if (!p || !p->mpd) {
        return ZM_ERR_INVALID;
    }
    if (p->have_last) {
        dash_mpd_input(p->mpd, 0, NULL, 0, p->last_pts, p->last_dts, 0);
    }
    return dash_emit_playlist(p) == 0 ? ZM_OK : ZM_ERR_IO;
}

static void dash_free_state(struct zm_dash_packager *p)
{
    if (!p) {
        return;
    }
    if (p->mpd) {
        dash_mpd_destroy(p->mpd);
    }
    free(p->conv);
    free(p);
}

static void dash_close(zm_packager *base)
{
    struct zm_dash_packager *p = base ? (struct zm_dash_packager *)base->impl : NULL;

    dash_free_state(p);
    free(base);
}

static const zm_packager_ops k_dash_ops = {
    .write = dash_write,
    .flush = dash_flush,
    .close = dash_close,
};

zm_packager *zm_dash_packager_open(const zm_track *tracks, size_t track_count,
                                     const zm_packager_cfg *cfg, const zm_packager_cb *cb,
                                     void *ctx)
{
    struct zm_dash_packager *p;
    zm_packager *base;
    size_t i;

    if (!tracks || track_count == 0 || !cb) {
        return NULL;
    }
    p = (struct zm_dash_packager *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->cb = *cb;
    p->ctx = ctx;
    p->live = cfg ? cfg->live : 1;
    p->mpd = dash_mpd_create(p->live ? DASH_DYNAMIC : DASH_STATIC, dash_on_segment, p);
    if (!p->mpd) {
        free(p);
        return NULL;
    }

    for (i = 0; i < track_count && p->track_count < ZM_DASH_MAX_TRACKS; ++i) {
        const zm_track *src = &tracks[i];
        const char *prefix = (cfg && cfg->prefix && cfg->prefix[0]) ? cfg->prefix : NULL;
        const zm_codec_desc *desc = zm_codec_get(src->codec);
        zm_dash_wtrack *dst = &p->tracks[p->track_count];
        int adapt;

        if (!desc || desc->map.mov_object == 0) {
            continue;
        }
        memset(dst, 0, sizeof(*dst));
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
            adapt = dash_mpd_add_video_adaptation_set(p->mpd, prefix ? prefix : "video",
                                                      (uint8_t)desc->map.mov_object, src->width,
                                                      src->height, src->config.data,
                                                      src->config.len);
        } else if (src->kind == ZM_KIND_AUDIO) {
            adapt = dash_mpd_add_audio_adaptation_set(
                p->mpd, prefix ? prefix : "audio", (uint8_t)desc->map.mov_object, src->channels,
                src->bits_per_sample, src->sample_rate, src->config.data, src->config.len);
        } else {
            continue;
        }
        if (adapt < 0) {
            continue;
        }
        dst->adapt = adapt;
        ++p->track_count;
    }
    if (p->track_count == 0) {
        dash_free_state(p);
        return NULL;
    }

    base = (zm_packager *)calloc(1, sizeof(*base));
    if (!base) {
        dash_free_state(p);
        return NULL;
    }
    base->ops = &k_dash_ops;
    base->impl = p;
    return base;
}
