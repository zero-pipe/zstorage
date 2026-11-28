/**
 * @file client.c
 * @brief WebRTC 主动连接：WHEP client（拉流）与 WHIP client（推流）。
 */
#include "zcs/session/webrtc/webrtc_server.h"

#include "session/webrtc/client_http.h"
#include "protocol/webrtc/ice_internal.h"
#include "session/webrtc/session_internal.h"
#include "zcs/core/event.h"
#include "zcs/core/stream/hub.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/media/media_limits.h"
#include "zcs/play/dispatch.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZCS_WEBRTC_CLIENT_MAX 8
#define ZCS_WEBRTC_CLIENT_CONNECT_TIMEOUT_MS 45000u

typedef struct zcs_webrtc_client {
    char id[32];
    char kind[8];
    char url[512];
    char app[64];
    char stream[128];
    char token[256];
    zn_poller *poller;
    void *ssl_ctx;
    zcs_webrtc_session *sess;
    zcs_http_client *http;
    int state;
    int ice_connected;
    int dtls_ready;
    int media_started;
    int stop;
    uint64_t created_ms;
    char error[128];
    struct zcs_webrtc_client *next;
} zcs_webrtc_client;

static zcs_webrtc_client *g_clients;
static zb_mutex *g_client_mtx;
static unsigned g_client_no;
static zn_poller *g_client_poller;
static zn_poller_timer *g_client_tick;

static zcs_webrtc_client *client_find_locked(const char *id)
{
    zcs_webrtc_client *c;

    for (c = g_clients; c; c = c->next) {
        if (strcmp(c->id, id) == 0) {
            return c;
        }
    }
    return NULL;
}

static void client_destroy(zcs_webrtc_client *c)
{
    if (!c) {
        return;
    }
    if (c->http) {
        zcs_http_client_destroy(c->http);
        c->http = NULL;
    }
    if (c->sess) {
        zcs_webrtc_session_destroy(c->sess);
        c->sess = NULL;
    }
    free(c);
}

static void client_unlink(zcs_webrtc_client *c)
{
    zcs_webrtc_client **pp;

    if (!c || !g_client_mtx) {
        return;
    }
    zb_mutex_lock(g_client_mtx);
    for (pp = &g_clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            c->next = NULL;
            break;
        }
    }
    zb_mutex_unlock(g_client_mtx);
}

static void client_fail(zcs_webrtc_client *c, const char *reason)
{
    if (!c) {
        return;
    }
    snprintf(c->error, sizeof(c->error), "%s", reason ? reason : "failed");
    c->state = -1;
    zn_warn("[webrtc-client] %s %s: %s", c->kind, c->id, c->error);
    if (c->http) {
        zcs_http_client_destroy(c->http);
        c->http = NULL;
    }
    if (c->sess) {
        zcs_webrtc_session_destroy(c->sess);
        c->sess = NULL;
    }
}

static void client_refresh_state(zcs_webrtc_client *c)
{
    if (!c) {
        return;
    }
    if (!c->sess) {
        return;
    }
    c->ice_connected = c->sess->ice_host.ctx && zcs_webrtc_ice_connected(c->sess->ice_host.ctx);
    c->dtls_ready = c->sess->dtls_ready;
    c->media_started = c->sess->media_started;
    if (c->state >= 0) {
        if (c->media_started) {
            c->state = 3;
        } else if (c->dtls_ready) {
            c->state = 2;
        } else if (c->ice_connected) {
            c->state = 1;
        } else {
            c->state = 0;
        }
    }
}

static uint64_t client_tick_cb(void *user)
{
    zcs_webrtc_client *c;
    zcs_webrtc_client *dead[ZCS_WEBRTC_CLIENT_MAX];
    int n = 0;
    int i;
    uint64_t now = zn_monotonic_ms();

    (void)user;
    if (!g_client_mtx) {
        return 1000;
    }
    zb_mutex_lock(g_client_mtx);
    for (c = g_clients; c; c = c->next) {
        client_refresh_state(c);
        if (c->state >= 0 && !c->media_started &&
            now > c->created_ms &&
            now - c->created_ms > ZCS_WEBRTC_CLIENT_CONNECT_TIMEOUT_MS) {
            snprintf(c->error, sizeof(c->error), "connect timeout");
            c->state = -1;
            if (c->http) {
                zcs_http_client_destroy(c->http);
                c->http = NULL;
            }
            if (c->sess && n < ZCS_WEBRTC_CLIENT_MAX) {
                dead[n++] = c;
            }
        }
    }
    for (i = 0; i < n; ++i) {
        if (dead[i]->sess) {
            zcs_webrtc_session_destroy(dead[i]->sess);
            dead[i]->sess = NULL;
        }
        zn_warn("[webrtc-client] %s %s connect timeout", dead[i]->kind, dead[i]->id);
    }
    zb_mutex_unlock(g_client_mtx);
    return 1000;
}

static void client_ensure_tick(zn_poller *poller)
{
    if (!poller) {
        return;
    }
    g_client_poller = poller;
    if (!g_client_tick) {
        g_client_tick = zn_poller_do_delay(poller, 1000, client_tick_cb, NULL);
    }
}

static void client_on_answer(int status, const char *location, const char *body, size_t body_len,
                             void *user)
{
    zcs_webrtc_client *c = (zcs_webrtc_client *)user;

    if (!c) {
        return;
    }
    c->http = NULL;
    if (c->stop) {
        client_unlink(c);
        client_destroy(c);
        return;
    }
    if (status < 200 || status >= 300 || !body || body_len == 0) {
        char why[96];
        snprintf(why, sizeof(why), "http status %d", status);
        client_fail(c, why);
        return;
    }
    if (!c->sess ||
        zcs_webrtc_session_apply_client_answer(c->sess, body, body_len) != 0) {
        client_fail(c, "apply answer failed");
        return;
    }
    c->state = 1;
    zn_info("[webrtc-client] %s %s answer ok location=%s sdp=%zu", c->kind, c->id,
             location ? location : "-", body_len);
}

static zcs_webrtc_session *client_create_whip_session(zcs_webrtc_client *c)
{
    zcs_media_source *src;
    zcs_webrtc_session *sess;
    zcs_session_play_opts pcfg;

    src = zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, NULL, c->app, c->stream);
    if (!src || !src->gop_queue || zcs_media_source_is_vod(src)) {
        zn_warn("[webrtc-client] whip source offline %s/%s", c->app, c->stream);
        return NULL;
    }
    sess = zcs_webrtc_session_create(src, c->app, c->stream, c->poller);
    if (!sess) {
        return NULL;
    }
    zcs_session_dispatch_register_all();
    zcs_egress_source_init(&sess->play);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZCS_SESSION_WEBRTC;
    if (zcs_session_attach_play(ZCS_SESSION_WEBRTC, &sess->play, src, &pcfg) != ZN_OK) {
        zcs_webrtc_session_destroy(sess);
        return NULL;
    }
    if (zcs_webrtc_session_enable_client(sess) != 0) {
        zcs_session_detach_play(ZCS_SESSION_WEBRTC, &sess->play);
        zcs_webrtc_session_destroy(sess);
        return NULL;
    }
    zcs_media_source_reader_add(src);
    sess->play_reader_attached = 1;
    sess->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, "webrtc-client");
    return sess;
}

static zcs_webrtc_session *client_create_whep_session(zcs_webrtc_client *c)
{
    zcs_webrtc_session *sess;

    sess = zcs_webrtc_session_create_publish(NULL, c->app, c->stream, c->poller);
    if (!sess) {
        return NULL;
    }
    if (zcs_webrtc_session_enable_client(sess) != 0) {
        zcs_webrtc_session_destroy(sess);
        return NULL;
    }
    return sess;
}

static void client_launch_task(void *user)
{
    zcs_webrtc_client *c = (zcs_webrtc_client *)user;
    char offer[16384];
    size_t offer_len = 0;
    int recvonly;
    const char *kind;

    if (!c) {
        return;
    }
    kind = c->kind;
    recvonly = strcmp(kind, "whep") == 0;
    c->sess = recvonly ? client_create_whep_session(c) : client_create_whip_session(c);
    if (!c->sess) {
        client_fail(c, "session create failed");
        return;
    }
    if (zcs_webrtc_session_build_client_offer(c->sess, recvonly, offer, sizeof(offer),
                                              &offer_len) != 0) {
        client_fail(c, "build offer failed");
        return;
    }
    c->http = zcs_http_client_post(c->poller, c->url, "application/sdp", offer, offer_len,
                                   c->token[0] ? c->token : NULL, (struct zn_ssl_ctx *)c->ssl_ctx,
                                   10000, client_on_answer, c);
    if (!c->http) {
        client_fail(c, "http post failed");
        return;
    }
    zn_info("[webrtc-client] %s %s -> %s offer=%zu", kind, c->id, c->url, offer_len);
}

int zcs_webrtc_client_launch(const char *kind, const char *url, const char *app,
                             const char *stream, const char *token, void *ssl_ctx)
{
    zcs_webrtc_client *c;
    zcs_webrtc_server *svc = zcs_webrtc_server_instance();
    zn_poller *poller = zcs_webrtc_server_poller(svc);

    if (!kind || !url || !app || !stream || !poller) {
        return -1;
    }
    if (strcmp(kind, "whep") != 0 && strcmp(kind, "whip") != 0) {
        return -1;
    }
    if (!g_client_mtx) {
        g_client_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
        if (!g_client_mtx) {
            return -1;
        }
    }
    c = (zcs_webrtc_client *)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    c->poller = poller;
    c->ssl_ctx = ssl_ctx;
    snprintf(c->kind, sizeof(c->kind), "%s", kind);
    snprintf(c->url, sizeof(c->url), "%s", url);
    snprintf(c->app, sizeof(c->app), "%s", app);
    snprintf(c->stream, sizeof(c->stream), "%s", stream);
    snprintf(c->token, sizeof(c->token), "%s", token ? token : "");
    c->created_ms = zn_monotonic_ms();
    c->state = 0;
    zb_mutex_lock(g_client_mtx);
    snprintf(c->id, sizeof(c->id), "wc%u", ++g_client_no);
    c->next = g_clients;
    g_clients = c;
    zb_mutex_unlock(g_client_mtx);
    client_ensure_tick(poller);
    if (zn_poller_async(poller, client_launch_task, c, 0) != ZN_OK) {
        client_unlink(c);
        client_destroy(c);
        return -1;
    }
    return 0;
}

size_t zcs_webrtc_client_stats_all(zcs_webrtc_client_stats *out, size_t cap)
{
    zcs_webrtc_client *c;
    size_t n = 0;

    if (!out || cap == 0 || !g_client_mtx) {
        return 0;
    }
    zb_mutex_lock(g_client_mtx);
    for (c = g_clients; c && n < cap; c = c->next) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%s", c->id);
        snprintf(out[n].kind, sizeof(out[n].kind), "%s", c->kind);
        snprintf(out[n].url, sizeof(out[n].url), "%s", c->url);
        snprintf(out[n].app, sizeof(out[n].app), "%s", c->app);
        snprintf(out[n].stream, sizeof(out[n].stream), "%s", c->stream);
        if (c->sess) {
            snprintf(out[n].session_id, sizeof(out[n].session_id), "%s", c->sess->id);
        }
        out[n].state = c->state;
        out[n].ice_connected = c->ice_connected;
        out[n].dtls_ready = c->dtls_ready;
        out[n].media_started = c->media_started;
        out[n].created_ms = c->created_ms;
        snprintf(out[n].error, sizeof(out[n].error), "%s", c->error);
        ++n;
    }
    zb_mutex_unlock(g_client_mtx);
    return n;
}

static void client_stop_task(void *user)
{
    char *id = (char *)user;
    zcs_webrtc_client *c = NULL;

    if (!id) {
        return;
    }
    zb_mutex_lock(g_client_mtx);
    c = client_find_locked(id);
    zb_mutex_unlock(g_client_mtx);
    free(id);
    if (!c) {
        return;
    }
    c->stop = 1;
    client_unlink(c);
    zn_info("[webrtc-client] stop %s %s", c->kind, c->id);
    client_destroy(c);
}

int zcs_webrtc_client_stop(const char *id)
{
    char *copy;
    zcs_webrtc_server *svc;
    zn_poller *poller;

    if (!id || !g_client_mtx) {
        return -1;
    }
    copy = (char *)malloc(strlen(id) + 1);
    if (!copy) {
        return -1;
    }
    strcpy(copy, id);
    svc = zcs_webrtc_server_instance();
    poller = zcs_webrtc_server_poller(svc);
    if (poller && zn_poller_async(poller, client_stop_task, copy, 0) == ZN_OK) {
        return 0;
    }
    client_stop_task(copy);
    return 0;
}

void zcs_webrtc_client_shutdown(void)
{
    zcs_webrtc_client *c;
    zcs_webrtc_client *next;

    if (!g_client_mtx) {
        return;
    }
    zb_mutex_lock(g_client_mtx);
    c = g_clients;
    g_clients = NULL;
    zb_mutex_unlock(g_client_mtx);
    for (; c; c = next) {
        next = c->next;
        c->next = NULL;
        c->stop = 1;
        client_destroy(c);
    }
    if (g_client_tick && g_client_poller) {
        zn_poller_timer_cancel(g_client_tick);
        g_client_tick = NULL;
    }
}
