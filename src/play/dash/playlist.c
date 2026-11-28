/**
 * @file playlist.c
 * @brief DASH MPD 清单生成与分片登记。
 *
 * 存储模型：视频/音频各一个固定环形缓冲（video_segments/audio_segments），
 * 容量是 MPD 窗口的 2 倍——给“MPD 里还列着、但已被新分片挤出窗口”的分片留一段
 * 存活余量，否则播放器照 MPD 去取会 404。init 段（-init.m4v/-init.m4a）单独存。
 * 每次登记分片后立即重建 MPD 文本缓存，读请求只做一次 memcpy。
 *
 * 并发：写（on_segment，packager 线程）与读（HTTP 请求线程）都经 m->mtx 保护；
 * 提供 copy_/ref_ 两套读取：copy 拷贝字节，ref 直接增引用返回底层 zn_buf（省一次拷贝）。
 */
#include "zcs/play/dash/playlist.h"
#include "dash-mpd.h"
#include "dash-proto.h"
#include "zn/util/buf.h"
#include "zb/os/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

struct zcs_http_dash_playlist {
    dash_mpd_t *mpd;
    char prefix[64];
    unsigned seg_number;
    zcs_http_dash_segment init_video;
    zcs_http_dash_segment init_audio;
    zcs_http_dash_segment *video_segments;
    zcs_http_dash_segment *audio_segments;
    int video_seg_count;
    int video_seg_head;
    int audio_seg_count;
    int audio_seg_head;
    char *mpd_cache;
    size_t mpd_cache_cap;
    size_t mpd_cache_len;
    zb_mutex *mtx;
};

/* 重建 MPD 文本缓存（调用方持锁）。生成失败或超出缓冲（返回 ≥cap）时把长度置 0——
 * 读取方看到 0 会返回错误并触发重建，而不是返回半截 MPD。 */
static void dash_playlist_refresh_mpd_cache_locked(zcs_http_dash_playlist *m)
{
    size_t n;

    if (!m || !m->mpd) {
        return;
    }
    if (!m->mpd_cache) {
        m->mpd_cache_cap = 16384;
        m->mpd_cache = (char *)malloc(m->mpd_cache_cap);
        if (!m->mpd_cache) {
            return;
        }
    }
    n = dash_mpd_playlist(m->mpd, m->mpd_cache, m->mpd_cache_cap);
    if (n > 0 && n < m->mpd_cache_cap) {
        m->mpd_cache_len = n;
    } else {
        m->mpd_cache_len = 0;
    }
}

static size_t dash_seg_len(const zcs_http_dash_segment *s)
{
    return s && s->buf ? zn_buf_len(s->buf) : 0;
}

static void dash_seg_free(zcs_http_dash_segment *s)
{
    if (!s) {
        return;
    }
    if (s->buf) {
        zn_buf_unref(s->buf);
    }
    s->buf = NULL;
    s->name[0] = '\0';
}

static int dash_is_init_segment(const char *name)
{
    return name && strstr(name, "-init.");
}

static int dash_is_audio_segment(const char *name)
{
    return name && strstr(name, ".m4a");
}

static zcs_http_dash_segment *dash_init_slot(zcs_http_dash_playlist *m, const char *name)
{
    if (!m || !name) {
        return NULL;
    }
    if (strstr(name, "-init.m4a")) {
        return &m->init_audio;
    }
    if (strstr(name, "-init.m4v")) {
        return &m->init_video;
    }
    return NULL;
}

static void dash_slot_adopt(zcs_http_dash_segment *slot, const char *name, zn_buf *buf)
{
    if (!slot || !name || !buf || zn_buf_len(buf) == 0) {
        return;
    }
    dash_seg_free(slot);
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->buf = buf;
}

static int dash_slot_matches_name(const zcs_http_dash_segment *slot, const char *name)
{
    return slot && name && name[0] && slot->name[0] && strcmp(slot->name, name) == 0;
}

/* 登记一个媒体分片到对应环。先按名字找同槽位（同一分片重复推送时原地替换，幂等）；
 * 否则用空槽，没有空槽就按 head 轮转淘汰最旧。淘汰策略的选择见下方大注释
 * （不能用 mpd_cache 判活）。 */
static void dash_store_media_locked(zcs_http_dash_playlist *m, const char *name, zn_buf *buf)
{
    int is_audio = dash_is_audio_segment(name);
    zcs_http_dash_segment *slots = is_audio ? m->audio_segments : m->video_segments;
    int *head = is_audio ? &m->audio_seg_head : &m->video_seg_head;
    int *count = is_audio ? &m->audio_seg_count : &m->video_seg_count;
    zcs_http_dash_segment *slot;
    int i;
    int victim = -1;

    if (!slots || !buf) {
        return;
    }

    for (i = 0; i < (int)m->seg_number; ++i) {
        if (dash_slot_matches_name(&slots[i], name)) {
            dash_slot_adopt(&slots[i], name, buf);
            return;
        }
    }

    for (i = 0; i < (int)m->seg_number; ++i) {
        if (!slots[i].buf || slots[i].name[0] == '\0') {
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        /* 环大小是 MPD 窗口的 2 倍（见 create 里的 seg_number *= 2），
         * 直接轮转淘汰最老的即可。不能用 mpd_cache 判活：zm packager
         * 路径下 ZCS 自建的 m->mpd 没有喂入分片，缓存恒为空，会把第一个
         * 槽位当成过期反复覆盖，导致除最新分片外全部 404。 */
        victim = *head % (int)m->seg_number;
    }

    slot = &slots[victim];
    dash_slot_adopt(slot, name, buf);
    *head = (victim + 1) % (int)m->seg_number;
    if (*count < (int)m->seg_number) {
        ++*count;
    }
}

static void dash_store_segment_locked(zcs_http_dash_playlist *m, const char *name, zn_buf *buf)
{
    zcs_http_dash_segment *slot;

    if (!m || !name || !buf || zn_buf_len(buf) == 0) {
        return;
    }
    slot = dash_init_slot(m, name);
    if (slot) {
        dash_slot_adopt(slot, name, buf);
        return;
    }
    dash_store_media_locked(m, name, buf);
}

/* packager 回调：把新分片拷进自有 zn_buf 并登记、刷新 MPD。必须在入口就把数据拷走——
 * 回调返回后 data 指针即失效。锁内只做存储与 MPD 重建。 */
int zcs_http_dash_playlist_on_segment(void *param, int track, const void *data, size_t bytes,
                                      int64_t pts, int64_t dts, int64_t duration, const char *name)
{
    zcs_http_dash_playlist *m = (zcs_http_dash_playlist *)param;
    zn_buf *buf;

    (void)track;
    (void)pts;
    (void)dts;
    (void)duration;
    if (!m || !data || bytes == 0 || !name || !name[0]) {
        return -1;
    }

    buf = zn_buf_alloc(bytes);
    if (!buf) {
        return -1;
    }
    memcpy((void *)zn_buf_data(buf), data, bytes);
    zn_buf_set_len(buf, bytes);

    zb_mutex_lock(m->mtx);
    dash_store_segment_locked(m, name, buf);
    dash_playlist_refresh_mpd_cache_locked(m);
    zb_mutex_unlock(m->mtx);
    return 0;
}

zcs_http_dash_playlist *zcs_http_dash_playlist_create(const zcs_http_dash_playlist_opts *opts)
{
    zcs_http_dash_playlist *m = (zcs_http_dash_playlist *)calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }
    m->seg_number = opts && opts->segment_count ? opts->segment_count : 12;
    if (m->seg_number < 6) {
        m->seg_number = 6;
    }
    /* 内存表容量 2×MPD 窗口，降低 MPD 仍引用时分片被环覆盖导致 404 */
    m->seg_number *= 2u;
    if (opts && opts->prefix && opts->prefix[0]) {
        snprintf(m->prefix, sizeof(m->prefix), "%s", opts->prefix);
    } else {
        strncpy(m->prefix, "live", sizeof(m->prefix) - 1);
    }

    m->mpd = dash_mpd_create(DASH_DYNAMIC, zcs_http_dash_playlist_on_segment, m);
    m->mtx = zb_mutex_create(0);
    m->video_segments =
        (zcs_http_dash_segment *)calloc(m->seg_number, sizeof(zcs_http_dash_segment));
    m->audio_segments =
        (zcs_http_dash_segment *)calloc(m->seg_number, sizeof(zcs_http_dash_segment));
    if (!m->mpd || !m->mtx || !m->video_segments || !m->audio_segments) {
        dash_mpd_destroy(m->mpd);
        zb_mutex_destroy(m->mtx);
        free(m->video_segments);
        free(m->audio_segments);
        free(m);
        return NULL;
    }
    return m;
}

void zcs_http_dash_playlist_destroy(zcs_http_dash_playlist *m)
{
    int i;

    if (!m) {
        return;
    }
    dash_mpd_destroy(m->mpd);
    zb_mutex_destroy(m->mtx);
    dash_seg_free(&m->init_video);
    dash_seg_free(&m->init_audio);
    if (m->video_segments) {
        for (i = 0; i < (int)m->seg_number; ++i) {
            dash_seg_free(&m->video_segments[i]);
        }
        free(m->video_segments);
    }
    if (m->audio_segments) {
        for (i = 0; i < (int)m->seg_number; ++i) {
            dash_seg_free(&m->audio_segments[i]);
        }
        free(m->audio_segments);
    }
    free(m->mpd_cache);
    free(m);
}

struct dash_mpd_t *zcs_http_dash_playlist_mpd(zcs_http_dash_playlist *m)
{
    return m ? m->mpd : NULL;
}

static int dash_segment_slot_matches(const zcs_http_dash_segment *s, const char *name)
{
    return s && name && name[0] && s->buf && dash_seg_len(s) > 0 && s->name[0] &&
           strcmp(s->name, name) == 0;
}

static int dash_ring_has_name(const zcs_http_dash_segment *slots, int nslots, const char *name)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        if (dash_segment_slot_matches(&slots[i], name)) {
            return 1;
        }
    }
    return 0;
}

static int dash_segment_in_cache_locked(const zcs_http_dash_playlist *m, const char *name)
{
    if (!m || !name || !name[0]) {
        return 0;
    }
    if (dash_segment_slot_matches(&m->init_video, name) ||
        dash_segment_slot_matches(&m->init_audio, name)) {
        return 1;
    }
    if (dash_is_audio_segment(name)) {
        return dash_ring_has_name(m->audio_segments, (int)m->seg_number, name);
    }
    return dash_ring_has_name(m->video_segments, (int)m->seg_number, name);
}

static int dash_ext_matches(const char *name, const char *ext)
{
    size_t nlen;
    size_t elen;

    if (!name || !ext) {
        return 0;
    }
    nlen = strlen(name);
    elen = strlen(ext);
    if (nlen < elen) {
        return 0;
    }
    return strcmp(name + nlen - elen, ext) == 0;
}

/*
 * 从环里按“前缀 + 时间号 + 扩展名”挑出真实存在的分片名。
 * 先试精确名字 want；再退一步按数字部分（时间戳）匹配——因为不同 packager 生成的
 * 分片名里“前缀-数字”之外的中间部分可能不一致，只比数字更宽容。都不匹配返回 0。
 */
static int dash_pick_from_ring(const zcs_http_dash_playlist *m, const zcs_http_dash_segment *slots,
                               const char *prefix, size_t plen, const char *rest, const char *dot,
                               char *out, size_t cap)
{
    int i;
    int64_t want_ts;
    char *end;
    char want[128];

    (void)m;
    if (!prefix || !rest || !dot || !out || cap == 0) {
        return 0;
    }

    want_ts = strtoll(rest, &end, 10);
    if (!end || end == rest || strcmp(end, dot) != 0) {
        return 0;
    }

    snprintf(want, sizeof(want), "%.*s-%.*s%s", (int)plen, prefix, (int)(end - rest), rest, dot);

    for (i = 0; i < (int)m->seg_number; ++i) {
        const zcs_http_dash_segment *s = &slots[i];

        if (!s->buf || dash_seg_len(s) == 0 || !s->name[0] || dash_is_init_segment(s->name)) {
            continue;
        }
        if (strcmp(s->name, want) == 0) {
            snprintf(out, cap, "%s", s->name);
            return 1;
        }
        if (strncmp(s->name, prefix, plen) != 0 || s->name[plen] != '-') {
            continue;
        }
        if (strtoll(s->name + plen + 1, &end, 10) == want_ts && end && strcmp(end, dot) == 0) {
            snprintf(out, cap, "%s", s->name);
            return 1;
        }
    }
    return 0;
}

/* 把请求里的分片名解析成缓存里实际存在的名字：先精确命中；否则按“前缀-数字.扩展”
 * 归一后再到对应环里找（容忍客户端把分片名写得与 packager 生成的不完全一致）。
 * 返回 1 表示解析成功，结果写入 out。 */
static int dash_resolve_segment_name(zcs_http_dash_playlist *m, const char *req, char *out,
                                     size_t cap)
{
    const char *prefix;
    size_t plen;
    const char *rest;
    const char *dot;
    const zcs_http_dash_segment *slots;
    int found = 0;

    if (!m || !req || !out || cap == 0) {
        return 0;
    }

    zb_mutex_lock(m->mtx);
    if (dash_segment_in_cache_locked(m, req)) {
        snprintf(out, cap, "%s", req);
        zb_mutex_unlock(m->mtx);
        return 1;
    }

    prefix = m->prefix[0] ? m->prefix : "live";
    plen = strlen(prefix);
    if (strncmp(req, prefix, plen) != 0 || req[plen] != '-') {
        zb_mutex_unlock(m->mtx);
        return 0;
    }
    rest = req + plen + 1;
    dot = strrchr(rest, '.');
    if (!dot || dot == rest) {
        zb_mutex_unlock(m->mtx);
        return 0;
    }

    slots = dash_is_audio_segment(req) ? m->audio_segments : m->video_segments;
    found = dash_pick_from_ring(m, slots, prefix, plen, rest, dot, out, cap);
    zb_mutex_unlock(m->mtx);
    return found;
}

/* 是否至少已缓存一个媒体（非 init）视频分片——用于判断清单是否已“可播”，
 * 供上层在无分片时返回 503/稍后重试。 */
int zcs_http_dash_playlist_has_media_segment(const zcs_http_dash_playlist *m)
{
    int i;
    int found = 0;

    if (!m) {
        return 0;
    }
    zb_mutex_lock(m->mtx);
    for (i = 0; i < (int)m->seg_number; ++i) {
        const zcs_http_dash_segment *s = &m->video_segments[i];
        if (s->buf && dash_seg_len(s) > 0 && s->name[0] && !dash_is_init_segment(s->name)) {
            found = 1;
            break;
        }
    }
    zb_mutex_unlock(m->mtx);
    return found;
}

zn_err_t zcs_http_dash_playlist_copy_mpd(const zcs_http_dash_playlist *m, char *out, size_t cap,
                                          size_t *out_len)
{
    if (!m || !out || !out_len) {
        return ZN_ERR_INVALID;
    }
    *out_len = 0;
    zb_mutex_lock(m->mtx);
    if (m->mpd_cache_len == 0 || m->mpd_cache_len > cap) {
        zb_mutex_unlock(m->mtx);
        return ZN_ERR_INVALID;
    }
    memcpy(out, m->mpd_cache, m->mpd_cache_len);
    *out_len = m->mpd_cache_len;
    zb_mutex_unlock(m->mtx);
    return ZN_OK;
}

zn_err_t zcs_http_dash_playlist_build_mpd(zcs_http_dash_playlist *m, char *out, size_t cap,
                                           size_t *out_len)
{
    if (!m || !m->mpd || !out || !out_len) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(m->mtx);
    dash_playlist_refresh_mpd_cache_locked(m);
    if (m->mpd_cache_len == 0 || m->mpd_cache_len > cap) {
        zb_mutex_unlock(m->mtx);
        return ZN_ERR_INVALID;
    }
    memcpy(out, m->mpd_cache, m->mpd_cache_len);
    *out_len = m->mpd_cache_len;
    zb_mutex_unlock(m->mtx);
    return ZN_OK;
}

/* copy 版本：把环里命中的分片字节拷进调用方 buf（cap 不足返回 INVALID）。 */
static zn_err_t dash_copy_from_ring(const zcs_http_dash_segment *slots, int nslots,
                                     const char *resolved, uint8_t *buf, size_t cap,
                                     size_t *out_len)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        const zcs_http_dash_segment *s = &slots[i];
        if (!dash_segment_slot_matches(s, resolved)) {
            continue;
        }
        if (dash_seg_len(s) > cap) {
            return ZN_ERR_INVALID;
        }
        memcpy(buf, zn_buf_data(s->buf), dash_seg_len(s));
        *out_len = dash_seg_len(s);
        return ZN_OK;
    }
    return ZN_ERR_INVALID;
}

zn_err_t zcs_http_dash_playlist_copy_segment(zcs_http_dash_playlist *m, const char *name,
                                              uint8_t *buf, size_t cap, size_t *out_len)
{
    char resolved[128];
    zn_err_t err;

    if (!m || !name || !buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    *out_len = 0;
    if (!dash_resolve_segment_name(m, name, resolved, sizeof(resolved))) {
        return ZN_ERR_INVALID;
    }

    zb_mutex_lock(m->mtx);
    if (dash_segment_slot_matches(&m->init_video, resolved) ||
        dash_segment_slot_matches(&m->init_audio, resolved)) {
        const zcs_http_dash_segment *init_slots[2] = {&m->init_video, &m->init_audio};
        int j;

        for (j = 0; j < 2; ++j) {
            const zcs_http_dash_segment *s = init_slots[j];
            if (!dash_segment_slot_matches(s, resolved)) {
                continue;
            }
            if (dash_seg_len(s) > cap) {
                zb_mutex_unlock(m->mtx);
                return ZN_ERR_INVALID;
            }
            memcpy(buf, zn_buf_data(s->buf), dash_seg_len(s));
            *out_len = dash_seg_len(s);
            zb_mutex_unlock(m->mtx);
            return ZN_OK;
        }
    }

    if (dash_is_audio_segment(resolved)) {
        err =
            dash_copy_from_ring(m->audio_segments, (int)m->seg_number, resolved, buf, cap, out_len);
    } else {
        err =
            dash_copy_from_ring(m->video_segments, (int)m->seg_number, resolved, buf, cap, out_len);
    }
    zb_mutex_unlock(m->mtx);
    return err;
}

/* ref 版本：不拷贝，直接把底层 zn_buf 增引用交给调用方（由它负责 unref）。
 * 省一次整片拷贝，适合直接把分片交给 sendfile/零拷贝发送路径。 */
static zn_err_t dash_ref_from_ring(const zcs_http_dash_segment *slots, int nslots,
                                    const char *resolved, zn_buf **out_buf, size_t *out_len)
{
    int i;

    for (i = 0; i < nslots; ++i) {
        const zcs_http_dash_segment *s = &slots[i];
        if (!dash_segment_slot_matches(s, resolved)) {
            continue;
        }
        *out_buf = zn_buf_ref((zn_buf *)s->buf);
        *out_len = dash_seg_len(s);
        return ZN_OK;
    }
    return ZN_ERR_INVALID;
}

zn_err_t zcs_http_dash_playlist_ref_segment(zcs_http_dash_playlist *m, const char *name,
                                             zn_buf **out_buf, size_t *out_len)
{
    char resolved[128];
    zn_err_t err;

    if (!m || !name || !out_buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    *out_buf = NULL;
    *out_len = 0;
    if (!dash_resolve_segment_name(m, name, resolved, sizeof(resolved))) {
        return ZN_ERR_INVALID;
    }

    zb_mutex_lock(m->mtx);
    if (dash_segment_slot_matches(&m->init_video, resolved) ||
        dash_segment_slot_matches(&m->init_audio, resolved)) {
        const zcs_http_dash_segment *init_slots[2] = {&m->init_video, &m->init_audio};
        int j;

        for (j = 0; j < 2; ++j) {
            const zcs_http_dash_segment *s = init_slots[j];
            if (!dash_segment_slot_matches(s, resolved)) {
                continue;
            }
            *out_buf = zn_buf_ref((zn_buf *)s->buf);
            *out_len = dash_seg_len(s);
            zb_mutex_unlock(m->mtx);
            return ZN_OK;
        }
    }

    if (dash_is_audio_segment(resolved)) {
        err = dash_ref_from_ring(m->audio_segments, (int)m->seg_number, resolved, out_buf, out_len);
    } else {
        err = dash_ref_from_ring(m->video_segments, (int)m->seg_number, resolved, out_buf, out_len);
    }
    zb_mutex_unlock(m->mtx);
    return err;
}

int zcs_http_dash_playlist_segment_count(const zcs_http_dash_playlist *m)
{
    return m ? m->video_seg_count + m->audio_seg_count : 0;
}

const char *zcs_http_dash_playlist_prefix(const zcs_http_dash_playlist *m)
{
    return m && m->prefix[0] ? m->prefix : "live";
}
