/**
 * @file onvif_registry.c
 * @brief ONVIF 设备 → 拉流源 登记表。
 */
#include "zms/device/onvif/onvif_registry.h"
#include "zms/device/onvif/onvif.h"
#include "zms/pull/proxy.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zms_onvif_entry {
    zms_onvif_entry_info info;
    char user[64];
    char pass[128];
    zms_live_pull_proxy *proxy;
    struct zms_onvif_entry *next;
} zms_onvif_entry;

static zms_onvif_entry *g_head;
static zb_mutex *g_mtx;
static int g_inited;

void zms_onvif_registry_init(void)
{
    if (g_inited) {
        return;
    }
    g_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    g_inited = 1;
}

static void entry_destroy(zms_onvif_entry *e)
{
    if (!e) {
        return;
    }
    if (e->proxy) {
        zms_live_pull_proxy_stop(e->proxy);
        zms_live_pull_proxy_destroy(e->proxy);
        e->proxy = NULL;
    }
    free(e);
}

void zms_onvif_registry_fini(void)
{
    zms_onvif_entry *e;

    if (!g_inited) {
        return;
    }
    zb_mutex_lock(g_mtx);
    e = g_head;
    g_head = NULL;
    zb_mutex_unlock(g_mtx);
    while (e) {
        zms_onvif_entry *next = e->next;
        entry_destroy(e);
        e = next;
    }
    zb_mutex_destroy(g_mtx);
    g_mtx = NULL;
    g_inited = 0;
}

static zms_onvif_entry *find_key_locked(const char *key)
{
    zms_onvif_entry *e;
    for (e = g_head; e; e = e->next) {
        if (strcmp(e->info.key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

int zms_onvif_registry_add(const char *addr, const char *user, const char *pass, const char *profile,
                           const char *uri, const char *vhost, const char *app, const char *stream,
                           zn_poller_pool *pool, char *key_out, size_t key_cap)
{
    zms_live_pull_proxy_opts popts;
    zms_live_pull_proxy *proxy;
    zms_onvif_entry *e;
    zms_onvif_entry *old = NULL;
    char key[256];
    char uri_auth[ZMS_ONVIF_URI_MAX];

    if (!g_inited || !uri || !uri[0] || !pool || !app || !stream) {
        return -1;
    }
    zms_live_pull_proxy_make_key(vhost, app, stream, key, sizeof(key));
    if (zms_onvif_uri_with_auth(uri, user, pass, uri_auth, sizeof(uri_auth)) != 0) {
        snprintf(uri_auth, sizeof(uri_auth), "%s", uri);
    }

    memset(&popts, 0, sizeof(popts));
    popts.poller_pool = pool;
    popts.pull_url = uri_auth;
    popts.vhost = vhost;
    popts.app = app;
    popts.stream = stream;
    popts.retry_count = -1;
    popts.reconnect_delay_ms = 3000;

    proxy = zms_live_pull_proxy_create(&popts);
    if (!proxy) {
        zn_warn("onvif: create pull proxy failed key=%s", key);
        return -1;
    }
    if (zms_live_pull_proxy_start(proxy) != ZN_OK) {
        zn_warn("onvif: start pull proxy failed key=%s url=%s", key, uri_auth);
        zms_live_pull_proxy_destroy(proxy);
        return -1;
    }

    e = (zms_onvif_entry *)calloc(1, sizeof(*e));
    if (!e) {
        zms_live_pull_proxy_stop(proxy);
        zms_live_pull_proxy_destroy(proxy);
        return -1;
    }
    snprintf(e->info.key, sizeof(e->info.key), "%s", key);
    snprintf(e->info.addr, sizeof(e->info.addr), "%s", addr ? addr : "");
    snprintf(e->info.profile, sizeof(e->info.profile), "%s", profile ? profile : "");
    snprintf(e->info.uri, sizeof(e->info.uri), "%s", uri_auth);
    snprintf(e->info.vhost, sizeof(e->info.vhost), "%s", vhost ? vhost : "");
    snprintf(e->info.app, sizeof(e->info.app), "%s", app);
    snprintf(e->info.stream, sizeof(e->info.stream), "%s", stream);
    if (user) {
        snprintf(e->user, sizeof(e->user), "%s", user);
    }
    if (pass) {
        snprintf(e->pass, sizeof(e->pass), "%s", pass);
    }
    e->proxy = proxy;

    zb_mutex_lock(g_mtx);
    old = find_key_locked(key);
    if (old) {
        /* 从链表中摘除，锁外销毁 */
        zms_onvif_entry **pp = &g_head;
        while (*pp && *pp != old) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = old->next;
        }
        old->next = NULL;
    }
    e->next = g_head;
    g_head = e;
    zb_mutex_unlock(g_mtx);

    if (old) {
        entry_destroy(old);
    }
    if (key_out && key_cap) {
        snprintf(key_out, key_cap, "%s", key);
    }
    zn_info("onvif: add %s -> %s (%s)", key, uri_auth, addr ? addr : "?");
    return 0;
}

int zms_onvif_registry_del_key(const char *key)
{
    zms_onvif_entry *e = NULL;

    if (!g_inited || !key || !key[0]) {
        return 0;
    }
    zb_mutex_lock(g_mtx);
    {
        zms_onvif_entry **pp = &g_head;
        while (*pp && strcmp((*pp)->info.key, key) != 0) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            e = *pp;
            *pp = e->next;
            e->next = NULL;
        }
    }
    zb_mutex_unlock(g_mtx);
    if (!e) {
        return 0;
    }
    zn_info("onvif: del %s (%s)", key, e->info.addr);
    entry_destroy(e);
    return 1;
}

int zms_onvif_registry_del_addr(const char *addr, int *removed)
{
    zms_onvif_entry *list = NULL;
    zms_onvif_entry *e;
    int n = 0;

    if (removed) {
        *removed = 0;
    }
    if (!g_inited || !addr || !addr[0]) {
        return 0;
    }
    zb_mutex_lock(g_mtx);
    {
        zms_onvif_entry **pp = &g_head;
        while (*pp) {
            if (strcmp((*pp)->info.addr, addr) == 0) {
                e = *pp;
                *pp = e->next;
                e->next = list;
                list = e;
                ++n;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    zb_mutex_unlock(g_mtx);
    while (list) {
        zms_onvif_entry *next = list->next;
        entry_destroy(list);
        list = next;
    }
    if (removed) {
        *removed = n;
    }
    return n;
}

static int entry_online(const zms_onvif_entry *e)
{
    zms_media_source *s;
    if (!e || !e->proxy) {
        return 0;
    }
    s = zms_live_pull_proxy_source(e->proxy);
    if (!s) {
        return 0;
    }
    if (s->vod_buffer) {
        return 1;
    }
    return s->gop_queue && (s->publishing || s->has_video || s->has_audio);
}

int zms_onvif_registry_foreach(zms_onvif_registry_visit_fn fn, void *user)
{
    zms_onvif_entry_info *snap = NULL;
    size_t n = 0;
    size_t i;
    zms_onvif_entry *e;

    if (!g_inited || !fn) {
        return 0;
    }
    zb_mutex_lock(g_mtx);
    for (e = g_head; e; e = e->next) {
        ++n;
    }
    if (n > 0) {
        snap = (zms_onvif_entry_info *)calloc(n, sizeof(*snap));
    }
    if (snap) {
        i = 0;
        for (e = g_head; e && i < n; e = e->next) {
            snap[i] = e->info;
            snap[i].online = entry_online(e);
            ++i;
        }
        n = i;
    } else {
        n = 0;
    }
    zb_mutex_unlock(g_mtx);

    for (i = 0; i < n; ++i) {
        if (fn(&snap[i], user) != 0) {
            break;
        }
    }
    free(snap);
    return (int)n;
}

int zms_onvif_registry_count(void)
{
    int n = 0;
    zms_onvif_entry *e;

    if (!g_inited) {
        return 0;
    }
    zb_mutex_lock(g_mtx);
    for (e = g_head; e; e = e->next) {
        ++n;
    }
    zb_mutex_unlock(g_mtx);
    return n;
}
