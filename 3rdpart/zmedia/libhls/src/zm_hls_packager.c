#include "zm/packager/packager.h"

#include "zm/core/codec.h"

#include "hls-media.h"
#include "hls-m3u8.h"
#include "hls-param.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zm_hls_packager {
    hls_media_t *hls;
    hls_m3u8_t *m3u8;
    zm_packager_cb cb;
    void *ctx;
    int live;
    int eof;
    uint32_t segment_index;
    int video_psi;
    int audio_psi;
    uint8_t asc[64];
    size_t asc_len;
    uint8_t *adts;
    size_t adts_cap;
    int64_t last_pts;
    int64_t last_dts;
    int have_last;
};

static int hls_emit_playlist(struct zm_hls_packager *p, int eof)
{
    char text[8192];
    int rc;

    if (!p || !p->m3u8 || !p->cb.on_playlist) {
        return 0;
    }
    rc = hls_m3u8_playlist(p->m3u8, eof, text, sizeof(text));
    if (rc != 0) {
        return -1;
    }
    p->cb.on_playlist(p->ctx, "index.m3u8", text, strlen(text));
    return 0;
}

static int hls_on_segment(void *param, const void *data, size_t bytes, int64_t pts, int64_t dts,
                          int64_t duration)
{
    struct zm_hls_packager *p = (struct zm_hls_packager *)param;
    char name[64];
    int discontinuity = 0;

    (void)dts;
    if (!p || !data || bytes == 0) {
        return 0;
    }
    snprintf(name, sizeof(name), "seg%05u.ts", p->segment_index++);
    if (p->cb.on_segment) {
        p->cb.on_segment(p->ctx, name, data, bytes, pts, duration, discontinuity);
    }
    if (p->m3u8) {
        hls_m3u8_add(p->m3u8, name, pts, duration, discontinuity);
    }
    hls_emit_playlist(p, p->eof);
    return 0;
}

static size_t hls_build_adts(const uint8_t *asc, size_t asc_len, const uint8_t *raw,
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

static int hls_ensure_adts(struct zm_hls_packager *p, size_t cap)
{
    if (cap <= p->adts_cap) {
        return 0;
    }
    {
        uint8_t *q = (uint8_t *)realloc(p->adts, cap);
        if (!q) {
            return -1;
        }
        p->adts = q;
        p->adts_cap = cap;
    }
    return 0;
}

static int hls_write(zm_packager *base, const zm_packet *pkt)
{
    struct zm_hls_packager *p = base ? (struct zm_hls_packager *)base->impl : NULL;
    const uint8_t *data;
    size_t len;
    int psi;
    int flags = 0;
    int rc;

    if (!p || !p->hls || !pkt || !pkt->mem.data || pkt->mem.len == 0) {
        return ZM_ERR_INVALID;
    }
    if (pkt->flags & ZM_PKT_CONFIG) {
        if (pkt->codec == ZM_CODEC_AAC && pkt->mem.len <= sizeof(p->asc)) {
            memcpy(p->asc, pkt->mem.data, pkt->mem.len);
            p->asc_len = pkt->mem.len;
        }
        return ZM_OK;
    }

    psi = pkt->kind == ZM_KIND_VIDEO ? p->video_psi
                                      : (pkt->kind == ZM_KIND_AUDIO ? p->audio_psi : 0);
    if (psi == 0) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    data = pkt->mem.data;
    len = pkt->mem.len;
    if (pkt->codec == ZM_CODEC_AAC) {
        size_t need = len + 7;
        size_t n;
        if (hls_ensure_adts(p, need) != 0) {
            return ZM_ERR_NOMEM;
        }
        n = hls_build_adts(p->asc, p->asc_len, data, len, p->adts, p->adts_cap);
        if (n == 0) {
            return ZM_ERR_NOT_SUPPORTED;
        }
        data = p->adts;
        len = n;
    }
    if (pkt->flags & ZM_PKT_KEY) {
        flags |= HLS_FLAGS_KEYFRAME;
    }
    rc = hls_media_input(p->hls, psi, data, len, pkt->pts_ms, pkt->dts_ms, flags);
    if (rc == 0) {
        p->last_pts = pkt->pts_ms;
        p->last_dts = pkt->dts_ms;
        p->have_last = 1;
    }
    return rc == 0 ? ZM_OK : ZM_ERR_INVALID;
}

static int hls_flush(zm_packager *base, int force)
{
    struct zm_hls_packager *p = base ? (struct zm_hls_packager *)base->impl : NULL;

    if (!p || !p->hls) {
        return ZM_ERR_INVALID;
    }
    p->eof = force && !p->live;
    if (p->have_last) {
        hls_media_input(p->hls, 0, NULL, 0, p->last_pts, p->last_dts, 0);
    }
    return hls_emit_playlist(p, p->eof) == 0 ? ZM_OK : ZM_ERR_IO;
}

static void hls_free_state(struct zm_hls_packager *p)
{
    if (!p) {
        return;
    }
    if (p->hls) {
        hls_media_destroy(p->hls);
    }
    if (p->m3u8) {
        hls_m3u8_destroy(p->m3u8);
    }
    free(p->adts);
    free(p);
}

static void hls_close(zm_packager *base)
{
    struct zm_hls_packager *p = base ? (struct zm_hls_packager *)base->impl : NULL;

    hls_free_state(p);
    free(base);
}

static const zm_packager_ops k_hls_ops = {
    .write = hls_write,
    .flush = hls_flush,
    .close = hls_close,
};

zm_packager *zm_hls_packager_open(const zm_track *tracks, size_t track_count,
                                    const zm_packager_cfg *cfg, const zm_packager_cb *cb,
                                    void *ctx)
{
    struct zm_hls_packager *p;
    zm_packager *base;
    size_t i;

    if (!tracks || track_count == 0 || !cb) {
        return NULL;
    }
    p = (struct zm_hls_packager *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->cb = *cb;
    p->ctx = ctx;
    p->live = cfg ? cfg->live : 1;
    p->hls = hls_media_create(cfg ? cfg->segment_ms : 0, hls_on_segment, p);
    {
        /* hls_m3u8_create 的第一个参数是 live 窗口大小，不是布尔值。 */
        int live_window = 0;
        if (p->live) {
            live_window = (cfg && cfg->window_segments > 0) ? cfg->window_segments : HLS_LIVE_NUM;
        }
        p->m3u8 = hls_m3u8_create(live_window, 3);
    }
    if (!p->hls || !p->m3u8) {
        hls_free_state(p);
        return NULL;
    }

    for (i = 0; i < track_count; ++i) {
        const zm_track *t = &tracks[i];
        const zm_codec_desc *desc = zm_codec_get(t->codec);
        int psi;

        if (!desc || desc->map.psi == 0) {
            continue;
        }
        psi = desc->map.psi;
        if (hls_media_add_stream(p->hls, psi, t->config.data, t->config.len) < 0) {
            continue;
        }
        if (t->kind == ZM_KIND_VIDEO) {
            p->video_psi = psi;
        } else if (t->kind == ZM_KIND_AUDIO) {
            p->audio_psi = psi;
            if (t->codec == ZM_CODEC_AAC && t->config.fmt == ZM_CONFIG_ASC &&
                t->config.len <= sizeof(p->asc)) {
                memcpy(p->asc, t->config.data, t->config.len);
                p->asc_len = t->config.len;
            }
        }
    }
    if (!p->video_psi && !p->audio_psi) {
        hls_free_state(p);
        return NULL;
    }

    base = (zm_packager *)calloc(1, sizeof(*base));
    if (!base) {
        hls_free_state(p);
        return NULL;
    }
    base->ops = &k_hls_ops;
    base->impl = p;
    return base;
}
