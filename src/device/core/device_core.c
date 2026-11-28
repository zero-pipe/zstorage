/**
 * @file device_core.c
 * @brief 设备/通道/能力注册表实现。
 */
#include "zcs/device/core/device_core.h"

#include "zcs/core/stream/hub.h"
#include "zcs/core/stream/limits.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    zcs_device_channel_info items[ZCS_DEVICE_CORE_MAX_CHANNELS];
    size_t count;
    zb_mutex *mtx;
    int inited;
} device_core;

static device_core g_core;

static void ensure_init(void)
{
    if (g_core.inited) {
        return;
    }
    g_core.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    g_core.inited = g_core.mtx ? 1 : 0;
}

void zcs_device_core_init(void)
{
    ensure_init();
}

void zcs_device_core_fini(void)
{
    if (!g_core.inited) {
        return;
    }
    zb_mutex_lock(g_core.mtx);
    memset(g_core.items, 0, sizeof(g_core.items));
    g_core.count = 0;
    zb_mutex_unlock(g_core.mtx);
    zb_mutex_destroy(g_core.mtx);
    g_core.mtx = NULL;
    g_core.inited = 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!cap) {
        return;
    }
    snprintf(dst, cap, "%s", src ? src : "");
}

static int key_equal(const zcs_device_channel_info *a, const char *vhost, const char *app,
                     const char *stream)
{
    return strcmp(a->vhost, vhost) == 0 && strcmp(a->app, app) == 0 &&
           strcmp(a->stream, stream) == 0;
}

static zcs_device_channel_info *find_key_locked(const char *vhost, const char *app,
                                                const char *stream)
{
    size_t i;

    for (i = 0; i < g_core.count; ++i) {
        if (key_equal(&g_core.items[i], vhost, app, stream)) {
            return &g_core.items[i];
        }
    }
    return NULL;
}

static zcs_device_channel_info *find_id_locked(const char *id)
{
    size_t i;

    if (!id || !id[0]) {
        return NULL;
    }
    for (i = 0; i < g_core.count; ++i) {
        if (strcmp(g_core.items[i].id, id) == 0 ||
            (g_core.items[i].gb_id[0] && strcmp(g_core.items[i].gb_id, id) == 0)) {
            return &g_core.items[i];
        }
    }
    return NULL;
}

static void fill_key(zcs_device_channel_info *ch, const char *vhost, const char *app,
                     const char *stream)
{
    char vh[ZCS_DEVICE_VHOST_MAX];

    zcs_vhost_normalize(vhost, vh, sizeof(vh));
    copy_str(ch->vhost, sizeof(ch->vhost), vh);
    copy_str(ch->app, sizeof(ch->app), app);
    copy_str(ch->stream, sizeof(ch->stream), stream);
}

/** 为发现通道生成唯一 id（app/stream，冲突时加 #n）。 */
static void make_discovered_id_locked(zcs_device_channel_info *ch)
{
    char base[ZCS_DEVICE_ID_MAX];
    unsigned seq = 0;

    snprintf(base, sizeof(base), "%s/%s", ch->app, ch->stream);
    copy_str(ch->id, sizeof(ch->id), base);
    while (find_id_locked(ch->id) && find_id_locked(ch->id) != ch) {
        ++seq;
        snprintf(ch->id, sizeof(ch->id), "%s#%u", base, seq);
        if (seq > 99) {
            break;
        }
    }
}

int zcs_device_core_register(const zcs_device_channel_info *in)
{
    zcs_device_channel_info *ch;
    zcs_device_channel_info tmp;

    if (!in || !in->app[0] || !in->stream[0]) {
        return -1;
    }
    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    tmp = *in;
    fill_key(&tmp, in->vhost, in->app, in->stream);
    if (!tmp.id[0]) {
        if (tmp.gb_id[0]) {
            copy_str(tmp.id, sizeof(tmp.id), tmp.gb_id);
        } else {
            snprintf(tmp.id, sizeof(tmp.id), "%s/%s", tmp.app, tmp.stream);
        }
    }
    if (!tmp.name[0]) {
        copy_str(tmp.name, sizeof(tmp.name), tmp.id);
    }
    tmp.flags |= ZCS_DEV_CH_CONFIGURED;
    if (!tmp.caps) {
        tmp.caps = ZCS_DEV_CAP_LIVE;
    }

    zb_mutex_lock(g_core.mtx);
    ch = find_key_locked(tmp.vhost, tmp.app, tmp.stream);
    if (!ch) {
        zcs_device_channel_info *by_id = find_id_locked(tmp.id);
        if (by_id && !key_equal(by_id, tmp.vhost, tmp.app, tmp.stream)) {
            zb_mutex_unlock(g_core.mtx);
            zn_warn("device_core: id %s already used by %s/%s/%s", tmp.id, by_id->vhost,
                     by_id->app, by_id->stream);
            return -1;
        }
        if (g_core.count >= ZCS_DEVICE_CORE_MAX_CHANNELS) {
            zb_mutex_unlock(g_core.mtx);
            return -1;
        }
        ch = &g_core.items[g_core.count++];
        memset(ch, 0, sizeof(*ch));
    }
    {
        unsigned flags = ch->flags | tmp.flags;
        unsigned caps = ch->caps | tmp.caps;
        int online = ch->online;
        *ch = tmp;
        ch->flags = flags;
        ch->caps = caps;
        ch->online = online || tmp.online;
    }
    zb_mutex_unlock(g_core.mtx);
    return 0;
}

int zcs_device_core_unregister(const char *id)
{
    size_t i;

    if (!id || !id[0]) {
        return -1;
    }
    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    zb_mutex_lock(g_core.mtx);
    for (i = 0; i < g_core.count; ++i) {
        zcs_device_channel_info *ch = &g_core.items[i];
        if ((strcmp(ch->id, id) == 0 || (ch->gb_id[0] && strcmp(ch->gb_id, id) == 0)) &&
            (ch->flags & ZCS_DEV_CH_CONFIGURED)) {
            if (i + 1 < g_core.count) {
                memmove(&g_core.items[i], &g_core.items[i + 1],
                        (g_core.count - i - 1) * sizeof(g_core.items[0]));
            }
            --g_core.count;
            memset(&g_core.items[g_core.count], 0, sizeof(g_core.items[0]));
            zb_mutex_unlock(g_core.mtx);
            return 0;
        }
    }
    zb_mutex_unlock(g_core.mtx);
    return -1;
}

int zcs_device_core_get(const char *id, zcs_device_channel_info *out)
{
    zcs_device_channel_info *ch;

    if (!id || !out) {
        return -1;
    }
    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    zb_mutex_lock(g_core.mtx);
    ch = find_id_locked(id);
    if (ch) {
        *out = *ch;
    }
    zb_mutex_unlock(g_core.mtx);
    return ch ? 0 : -1;
}

int zcs_device_core_get_key(const char *vhost, const char *app, const char *stream,
                            zcs_device_channel_info *out)
{
    char vh[ZCS_DEVICE_VHOST_MAX];
    zcs_device_channel_info *ch;

    if (!app || !stream || !out) {
        return -1;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));
    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    zb_mutex_lock(g_core.mtx);
    ch = find_key_locked(vh, app, stream);
    if (ch) {
        *out = *ch;
    }
    zb_mutex_unlock(g_core.mtx);
    return ch ? 0 : -1;
}

/* ── 在线流同步 ─────────────────────────────────────────────────────────── */

typedef struct {
    char vhost[ZCS_DEVICE_VHOST_MAX];
    char app[ZCS_DEVICE_APP_MAX];
    char stream[ZCS_DEVICE_STREAM_MAX];
    int publishing;
} live_key;

typedef struct {
    live_key *keys;
    size_t count;
    size_t cap;
} key_list;

static int source_playable(const zcs_media_source *s)
{
    if (!s || !s->gop_queue || s->vod_buffer) {
        return 0;
    }
    return s->publishing || ((s->has_video || s->has_audio) && s->video.ready);
}

static int collect_visit(zcs_media_source *s, void *user)
{
    key_list *list = (key_list *)user;

    if (!source_playable(s) || list->count >= list->cap) {
        return 0;
    }
    {
        live_key *k = &list->keys[list->count++];
        copy_str(k->vhost, sizeof(k->vhost), s->vhost);
        copy_str(k->app, sizeof(k->app), s->app);
        copy_str(k->stream, sizeof(k->stream), s->stream);
        k->publishing = s->publishing;
    }
    return 0;
}

static int key_seen(const key_list *list, const char *vhost, const char *app, const char *stream)
{
    size_t i;

    for (i = 0; i < list->count; ++i) {
        if (strcmp(list->keys[i].vhost, vhost) == 0 && strcmp(list->keys[i].app, app) == 0 &&
            strcmp(list->keys[i].stream, stream) == 0) {
            return 1;
        }
    }
    return 0;
}

int zcs_device_core_sync_live(void)
{
    key_list list;
    size_t i;
    int total;

    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    list.cap = ZCS_DEVICE_CORE_MAX_CHANNELS;
    list.count = 0;
    list.keys = (live_key *)calloc(list.cap, sizeof(live_key));
    if (!list.keys) {
        return -1;
    }
    /* 先在 hub 锁内收集，避免 core → hub 的嵌套加锁方向 */
    zcs_media_source_foreach(collect_visit, &list, NULL);

    zb_mutex_lock(g_core.mtx);
    /* 配置通道先置离线；发现通道若已不在线则删除 */
    for (i = 0; i < g_core.count;) {
        zcs_device_channel_info *ch = &g_core.items[i];
        int seen = key_seen(&list, ch->vhost, ch->app, ch->stream);

        if (ch->flags & ZCS_DEV_CH_CONFIGURED) {
            ch->online = 0;
        } else if ((ch->flags & ZCS_DEV_CH_DISCOVERED) && !seen) {
            if (i + 1 < g_core.count) {
                memmove(&g_core.items[i], &g_core.items[i + 1],
                        (g_core.count - i - 1) * sizeof(g_core.items[0]));
            }
            --g_core.count;
            memset(&g_core.items[g_core.count], 0, sizeof(g_core.items[0]));
            continue;
        }
        ++i;
    }
    /* 在线流：刷新已注册通道，或生成发现通道 */
    for (i = 0; i < list.count; ++i) {
        live_key *k = &list.keys[i];
        zcs_device_channel_info *ch = find_key_locked(k->vhost, k->app, k->stream);

        if (ch) {
            ch->online = k->publishing;
            ch->flags |= ZCS_DEV_CH_ONVIF;
            continue;
        }
        if (g_core.count >= ZCS_DEVICE_CORE_MAX_CHANNELS) {
            break;
        }
        ch = &g_core.items[g_core.count++];
        memset(ch, 0, sizeof(*ch));
        copy_str(ch->vhost, sizeof(ch->vhost), k->vhost);
        copy_str(ch->app, sizeof(ch->app), k->app);
        copy_str(ch->stream, sizeof(ch->stream), k->stream);
        snprintf(ch->name, sizeof(ch->name), "%s/%s", k->app, k->stream);
        ch->flags = ZCS_DEV_CH_DISCOVERED | ZCS_DEV_CH_ONVIF;
        ch->caps = ZCS_DEV_CAP_LIVE;
        ch->online = k->publishing;
        make_discovered_id_locked(ch);
    }
    total = (int)g_core.count;
    zb_mutex_unlock(g_core.mtx);
    free(list.keys);
    return total;
}

int zcs_device_core_foreach(unsigned require_caps, unsigned require_flags,
                            zcs_device_channel_visit_fn fn, void *user)
{
    size_t i;
    int n = 0;

    if (!fn) {
        return -1;
    }
    ensure_init();
    if (!g_core.mtx) {
        return -1;
    }
    zb_mutex_lock(g_core.mtx);
    for (i = 0; i < g_core.count; ++i) {
        const zcs_device_channel_info *ch = &g_core.items[i];
        if ((ch->caps & require_caps) != require_caps) {
            continue;
        }
        if ((ch->flags & require_flags) != require_flags) {
            continue;
        }
        ++n;
        if (fn(ch, user) != 0) {
            break;
        }
    }
    zb_mutex_unlock(g_core.mtx);
    return n;
}

int zcs_device_core_any_cap(unsigned caps)
{
    return zcs_device_core_count(caps, 0) > 0;
}

unsigned zcs_device_core_caps(void)
{
    unsigned caps = 0;
    size_t i;

    ensure_init();
    if (!g_core.mtx) {
        return 0;
    }
    zb_mutex_lock(g_core.mtx);
    for (i = 0; i < g_core.count; ++i) {
        caps |= g_core.items[i].caps;
    }
    zb_mutex_unlock(g_core.mtx);
    return caps;
}

size_t zcs_device_core_count(unsigned require_caps, unsigned require_flags)
{
    size_t i;
    size_t n = 0;

    ensure_init();
    if (!g_core.mtx) {
        return 0;
    }
    zb_mutex_lock(g_core.mtx);
    for (i = 0; i < g_core.count; ++i) {
        const zcs_device_channel_info *ch = &g_core.items[i];
        if ((ch->caps & require_caps) == require_caps &&
            (ch->flags & require_flags) == require_flags) {
            ++n;
        }
    }
    zb_mutex_unlock(g_core.mtx);
    return n;
}
