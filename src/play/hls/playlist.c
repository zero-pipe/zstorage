/**
 * @file playlist.c
 * @brief HLS m3u8 清单生成与分片登记。
 */
#include "zcs/play/hls/playlist.h"

#include "zcs/media/container/hls/hls_playlist.h"

#include "zn/util/buf.h"
#include "zb/os/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLS_VOD_DEFAULT_SEG_MS 8333u

struct zcs_http_hls_playlist {
    zcs_hls_m3u8 *m3u8;
    float seg_duration_sec;
    unsigned seg_number;
    uint64_t file_index;
    zcs_http_hls_segment *segments;
    int seg_count;
    int seg_head;
    int use_fmp4;
    zn_buf *init_seg;
    zb_mutex *mtx;
};

static size_t hls_seg_len(const zcs_http_hls_segment *s)
{
    return s && s->buf ? zn_buf_len(s->buf) : 0;
}

static void seg_free(zcs_http_hls_segment *s)
{
    if (!s) {
        return;
    }
    if (s->buf) {
        zn_buf_unref(s->buf);
    }
    s->buf = NULL;
    s->name[0] = '\0';
    s->dur_ms = 0;
}

static void store_segment_locked_buf(zcs_http_hls_playlist *m, const char *name, zn_buf *buf,
                                     int64_t pts_ms, int64_t duration_ms, int discontinuity)
{
    zcs_http_hls_segment *slot;
    uint32_t dur;

    if (!m || !name || !buf || zn_buf_len(buf) == 0) {
        return;
    }

    dur = duration_ms > 0 ? (uint32_t)duration_ms : (uint32_t)(m->seg_duration_sec * 1000.f);
    if (dur == 0) {
        dur = (uint32_t)(m->seg_duration_sec * 1000.f);
    }
    if (dur == 0) {
        dur = 2000;
    }

    slot = &m->segments[m->seg_head % (int)m->seg_number];
    seg_free(slot);
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->dur_ms = dur;
    slot->buf = buf;

    if (m->m3u8) {
        (void)zcs_hls_m3u8_add(m->m3u8, name, pts_ms, (int64_t)dur, discontinuity);
    }

    m->seg_head = (m->seg_head + 1) % (int)m->seg_number;
    if (m->seg_count < (int)m->seg_number) {
        ++m->seg_count;
    }
}

zcs_http_hls_playlist *zcs_http_hls_playlist_create(const zcs_http_hls_playlist_opts *opts)
{
    zcs_http_hls_playlist *m = (zcs_http_hls_playlist *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->seg_duration_sec =
        opts && opts->segment_duration_sec > 0.f ? opts->segment_duration_sec : 2.f;
    m->seg_number = opts && opts->segment_count ? opts->segment_count : 3;
    if (m->seg_number == 0) {
        m->seg_number = 3;
    }
    m->use_fmp4 = opts && opts->use_fmp4 ? 1 : 0;

    m->m3u8 = zcs_hls_m3u8_create((int)m->seg_number, m->use_fmp4 ? 7 : 3);
    if (m->use_fmp4 && m->m3u8) {
        (void)zcs_hls_m3u8_set_x_map(m->m3u8, "init.mp4");
    }
    m->mtx = zb_mutex_create(0);
    m->segments = (zcs_http_hls_segment *)calloc(m->seg_number, sizeof(zcs_http_hls_segment));
    if (!m->m3u8 || !m->mtx || !m->segments) {
        zcs_hls_m3u8_destroy(m->m3u8);
        zb_mutex_destroy(m->mtx);
        free(m->segments);
        free(m);
        return NULL;
    }
    return m;
}

void zcs_http_hls_playlist_destroy(zcs_http_hls_playlist *m)
{
    if (!m) {
        return;
    }
    zb_mutex_lock(m->mtx);
    for (unsigned i = 0; i < m->seg_number; ++i) {
        seg_free(&m->segments[i]);
    }
    if (m->init_seg) {
        zn_buf_unref(m->init_seg);
    }
    m->init_seg = NULL;
    zb_mutex_unlock(m->mtx);
    zcs_hls_m3u8_destroy(m->m3u8);
    free(m->segments);
    zb_mutex_destroy(m->mtx);
    free(m);
}

void zcs_http_hls_playlist_reset(zcs_http_hls_playlist *m)
{
    if (!m) {
        return;
    }
    zb_mutex_lock(m->mtx);
    for (unsigned i = 0; i < m->seg_number; ++i) {
        seg_free(&m->segments[i]);
    }
    m->seg_count = 0;
    m->seg_head = 0;
    m->file_index = 0;
    if (m->init_seg) {
        zn_buf_unref(m->init_seg);
        m->init_seg = NULL;
    }
    if (m->m3u8) {
        zcs_hls_m3u8_destroy(m->m3u8);
        m->m3u8 = zcs_hls_m3u8_create((int)m->seg_number, m->use_fmp4 ? 7 : 3);
        if (m->use_fmp4 && m->m3u8) {
            (void)zcs_hls_m3u8_set_x_map(m->m3u8, "init.mp4");
        }
    }
    zb_mutex_unlock(m->mtx);
}

zn_err_t zcs_http_hls_playlist_push_segment_buf(zcs_http_hls_playlist *m, zn_buf *buf,
                                                 int64_t pts_ms, int64_t dts_ms,
                                                 uint64_t duration_ms, int discontinuity)
{
    char name[64];

    if (!m || !buf || zn_buf_len(buf) == 0) {
        return ZN_ERR_INVALID;
    }
    (void)dts_ms;

    if (duration_ms == 0) {
        duration_ms = (uint64_t)(m->seg_duration_sec * 1000.f);
    }
    if (duration_ms == 0) {
        duration_ms = 2000;
    }

    snprintf(name, sizeof(name), m->use_fmp4 ? "%llu.m4s" : "%llu.ts",
             (unsigned long long)m->file_index++);

    zb_mutex_lock(m->mtx);
    store_segment_locked_buf(m, name, buf, pts_ms, (int64_t)duration_ms, discontinuity);
    zb_mutex_unlock(m->mtx);
    return ZN_OK;
}

zn_err_t zcs_http_hls_playlist_push_segment(zcs_http_hls_playlist *m, const void *data, size_t len,
                                             int64_t pts_ms, int64_t dts_ms, uint64_t duration_ms,
                                             int discontinuity)
{
    zn_buf *buf;
    zn_err_t err;

    if (!m || !data || len == 0) {
        return ZN_ERR_INVALID;
    }

    buf = zn_buf_alloc(len);
    if (!buf) {
        return ZN_ERR_NOMEM;
    }
    memcpy((void *)zn_buf_data(buf), data, len);
    zn_buf_set_len(buf, len);
    err =
        zcs_http_hls_playlist_push_segment_buf(m, buf, pts_ms, dts_ms, duration_ms, discontinuity);
    if (err != ZN_OK) {
        zn_buf_unref(buf);
    }
    return err;
}

zn_err_t zcs_http_hls_playlist_build_m3u8(const zcs_http_hls_playlist *m, char *out, size_t cap,
                                           size_t *out_len)
{
    if (!m || !m->m3u8 || !out || cap < 32) {
        return ZN_ERR_INVALID;
    }

    zb_mutex_lock(m->mtx);
    int r = zcs_hls_m3u8_playlist(m->m3u8, 0, out, cap);
    zb_mutex_unlock(m->mtx);
    if (r != 0) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    if (out_len) {
        *out_len = strlen(out);
    }
    return ZN_OK;
}

zn_err_t zcs_http_hls_playlist_copy_segment(zcs_http_hls_playlist *m, const char *name,
                                             uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!m || !name || !buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(m->mtx);
    for (int i = 0; i < m->seg_count; ++i) {
        int idx = (m->seg_head - m->seg_count + i + (int)m->seg_number) % (int)m->seg_number;
        const zcs_http_hls_segment *s = &m->segments[idx];
        if (s->name[0] && strcmp(s->name, name) == 0 && s->buf && hls_seg_len(s) > 0) {
            size_t slen = hls_seg_len(s);
            if (slen > cap) {
                zb_mutex_unlock(m->mtx);
                return ZN_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(buf, zn_buf_data(s->buf), slen);
            *out_len = slen;
            zb_mutex_unlock(m->mtx);
            return ZN_OK;
        }
    }
    zb_mutex_unlock(m->mtx);
    return ZN_ERR_INVALID;
}

int zcs_http_hls_playlist_use_fmp4(const zcs_http_hls_playlist *m)
{
    return m && m->use_fmp4 ? 1 : 0;
}

zn_err_t zcs_http_hls_playlist_store_init_segment(zcs_http_hls_playlist *m, zn_buf *buf)
{
    if (!m || !buf || zn_buf_len(buf) == 0) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(m->mtx);
    if (m->init_seg) {
        zn_buf_unref(m->init_seg);
    }
    m->init_seg = buf;
    zb_mutex_unlock(m->mtx);
    return ZN_OK;
}

zn_err_t zcs_http_hls_playlist_ref_segment(zcs_http_hls_playlist *m, const char *name,
                                            zn_buf **out_buf, size_t *out_len)
{
    if (!m || !name || !out_buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    *out_buf = NULL;
    *out_len = 0;
    zb_mutex_lock(m->mtx);
    if (m->use_fmp4 && strcmp(name, "init.mp4") == 0 && m->init_seg &&
        zn_buf_len(m->init_seg) > 0) {
        *out_buf = zn_buf_ref(m->init_seg);
        *out_len = zn_buf_len(m->init_seg);
        zb_mutex_unlock(m->mtx);
        return ZN_OK;
    }
    for (int i = 0; i < m->seg_count; ++i) {
        int idx = (m->seg_head - m->seg_count + i + (int)m->seg_number) % (int)m->seg_number;
        const zcs_http_hls_segment *s = &m->segments[idx];
        if (s->name[0] && strcmp(s->name, name) == 0 && s->buf && hls_seg_len(s) > 0) {
            *out_buf = zn_buf_ref((zn_buf *)s->buf);
            *out_len = hls_seg_len(s);
            zb_mutex_unlock(m->mtx);
            return ZN_OK;
        }
    }
    zb_mutex_unlock(m->mtx);
    return ZN_ERR_INVALID;
}

int zcs_http_hls_playlist_segment_count(const zcs_http_hls_playlist *m)
{
    int n;

    if (!m) {
        return 0;
    }
    zb_mutex_lock(m->mtx);
    n = m->seg_count;
    zb_mutex_unlock(m->mtx);
    return n;
}

int zcs_http_hls_playlist_latest_segment(const zcs_http_hls_playlist *m, char *out_name,
                                         size_t name_cap, size_t *out_len)
{
    int idx;
    const zcs_http_hls_segment *s;
    int ok = 0;

    if (!m || m->seg_count <= 0) {
        return 0;
    }
    zb_mutex_lock(m->mtx);
    idx = (m->seg_head - 1 + (int)m->seg_number) % (int)m->seg_number;
    s = &m->segments[idx];
    if (s->name[0] && s->buf && hls_seg_len(s) > 0 && (m->use_fmp4 || hls_seg_len(s) >= 188)) {
        if (out_name && name_cap) {
            snprintf(out_name, name_cap, "%s", s->name);
        }
        if (out_len) {
            *out_len = hls_seg_len(s);
        }
        ok = 1;
    }
    zb_mutex_unlock(m->mtx);
    return ok;
}

int zcs_http_hls_playlist_has_segment(const zcs_http_hls_playlist *m, const char *name)
{
    int found = 0;

    if (!m || !name || !name[0]) {
        return 0;
    }
    zb_mutex_lock(m->mtx);
    for (int i = 0; i < m->seg_count; ++i) {
        int idx = (m->seg_head - m->seg_count + i + (int)m->seg_number) % (int)m->seg_number;
        const zcs_http_hls_segment *s = &m->segments[idx];
        if (s->name[0] && strcmp(s->name, name) == 0 && s->buf && hls_seg_len(s) > 0 &&
            (m->use_fmp4 || hls_seg_len(s) >= 188)) {
            found = 1;
            break;
        }
    }
    zb_mutex_unlock(m->mtx);
    return found;
}

zn_err_t zcs_http_hls_playlist_build_vod_index_m3u8(const zcs_http_hls_playlist *m, char *out,
                                                     size_t cap, size_t *out_len,
                                                     uint64_t total_dur_ms, uint32_t seg_dur_ms,
                                                     int add_endlist)
{
    size_t n = 0;
    uint64_t nsegs;
    uint64_t i;
    int r;
    uint32_t max_dur_ms;

    if (!m || !out || cap < 64) {
        return ZN_ERR_INVALID;
    }
    if (seg_dur_ms == 0) {
        seg_dur_ms = (uint32_t)(m->seg_duration_sec * 1000.f);
    }
    if (seg_dur_ms == 0) {
        seg_dur_ms = HLS_VOD_DEFAULT_SEG_MS;
    }
    if (total_dur_ms == 0) {
        total_dur_ms = seg_dur_ms;
    }

    nsegs = (total_dur_ms + (uint64_t)seg_dur_ms - 1) / (uint64_t)seg_dur_ms;
    if (nsegs == 0) {
        nsegs = 1;
    }
    max_dur_ms = seg_dur_ms;

    r = snprintf(out, cap,
                 "#EXTM3U\r\n"
                 "#EXT-X-VERSION:3\r\n"
                 "#EXT-X-PLAYLIST-TYPE:VOD\r\n"
                 "#EXT-X-TARGETDURATION:%u\r\n"
                 "#EXT-X-MEDIA-SEQUENCE:0\r\n"
                 "#EXT-X-ALLOW-CACHE:YES\r\n",
                 (unsigned)((max_dur_ms + 999) / 1000));
    if (r <= 0 || (size_t)r >= cap) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    n = (size_t)r;

    for (i = 0; i < nsegs; ++i) {
        uint64_t remain =
            total_dur_ms > i * (uint64_t)seg_dur_ms ? total_dur_ms - i * (uint64_t)seg_dur_ms : 0;
        uint32_t dur_ms = remain >= seg_dur_ms ? seg_dur_ms : (uint32_t)remain;
        if (dur_ms == 0) {
            dur_ms = 1;
        }
        if (cap <= n) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        r = snprintf(out + n, cap - n, "#EXTINF:%.3f,\r\n%llu.ts\r\n", (double)dur_ms / 1000.0,
                     (unsigned long long)i);
        if (r <= 0 || (size_t)r >= cap - n) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        n += (size_t)r;
    }

    if (add_endlist && cap > n + 16) {
        r = snprintf(out + n, cap - n, "#EXT-X-ENDLIST\r\n");
        if (r <= 0 || (size_t)r >= cap - n) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        n += (size_t)r;
    }

    if (out_len) {
        *out_len = n;
    }
    return ZN_OK;
}
