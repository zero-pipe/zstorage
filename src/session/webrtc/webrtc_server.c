/**
 * @file webrtc_server.c
 * @brief WebRTC 服务实例管理。
 */
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/protocol/http/session_api.h"
#include "zms/media/media_limits.h"
#include "zn/net/tcp_server.h"
#include "protocol/webrtc/media_internal.h"
#include "protocol/webrtc/ice_internal.h"
#include "session/webrtc/session_internal.h"
#include "session/webrtc/signaling_client.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct client_autostart {
    char kind[8];
    char url[256];
    char app[64];
    char stream[128];
    char token[128];
    void *ssl_ctx;
} client_autostart;

static void client_autostart_cb(void *user)
{
    client_autostart *a = (client_autostart *)user;

    if (!a) {
        return;
    }
    if (zms_webrtc_client_launch(a->kind, a->url, a->app, a->stream,
                                 a->token[0] ? a->token : NULL, a->ssl_ctx) != 0) {
        zn_warn("[webrtc] autostart %s failed url=%s", a->kind, a->url);
    }
    free(a);
}

static void client_autostart_add(zn_poller *poller, const char *kind, const char *url,
                                 const char *app, const char *stream, const char *token,
                                 void *ssl_ctx)
{
    client_autostart *a;

    if (!poller || !url || !url[0]) {
        return;
    }
    a = (client_autostart *)calloc(1, sizeof(*a));
    if (!a) {
        return;
    }
    snprintf(a->kind, sizeof(a->kind), "%s", kind);
    snprintf(a->url, sizeof(a->url), "%s", url);
    snprintf(a->app, sizeof(a->app), "%s", app && app[0] ? app : "live");
    snprintf(a->stream, sizeof(a->stream), "%s", stream && stream[0] ? stream : "cam1");
    snprintf(a->token, sizeof(a->token), "%s", token ? token : "");
    a->ssl_ctx = ssl_ctx;
    if (zn_poller_do_delay(poller, 2000, client_autostart_cb, a) == NULL) {
        free(a);
    }
}
struct zms_webrtc_server {
    char host[64];
    char advertise_host[64];
    zn_poller *poller;
    uint16_t port_min;
    uint16_t port_max;
    zms_webrtc_ice_server ice_servers[2];
    size_t ice_server_count;
};

static zms_webrtc_server *g_webrtc_service;
zms_webrtc_server *zms_webrtc_server_instance(void)
{
    return g_webrtc_service;
}

void zms_webrtc_server_set_instance(zms_webrtc_server *srv)
{
    g_webrtc_service = srv;
}

const char *zms_webrtc_server_bind_host(const zms_webrtc_server *srv)
{
    return (srv && srv->host[0]) ? srv->host : "0.0.0.0";
}

const char *zms_webrtc_server_advertise_host(const zms_webrtc_server *srv)
{
    if (srv && srv->advertise_host[0]) {
        return srv->advertise_host;
    }
    return "127.0.0.1";
}

int zms_webrtc_server_port_range(const zms_webrtc_server *srv, uint16_t *out_min,
                                 uint16_t *out_max)
{
    if (!srv || !out_min || !out_max) {
        return 0;
    }
    *out_min = srv->port_min;
    *out_max = srv->port_max;
    return *out_min > 0 && *out_max >= *out_min;
}

zn_poller *zms_webrtc_server_poller(const zms_webrtc_server *srv)
{
    return srv ? srv->poller : NULL;
}

zn_poller *zms_webrtc_server_resolve_poller(zms_http_session *hs)
{
    zms_webrtc_server *svc = zms_webrtc_server_instance();
    if (hs && zms_http_session_tcp(hs)) {
        return zn_tcp_session_poller(zms_http_session_tcp(hs));
    }
    if (svc && svc->poller) {
        return svc->poller;
    }
    if (hs) {
        return zms_http_session_poller(hs);
    }
    return NULL;
}

static zms_webrtc_talk_audio_cb g_talk_cb;
static void *g_talk_user;

void zms_webrtc_server_set_talk_audio_cb(zms_webrtc_talk_audio_cb cb, void *user)
{
    g_talk_cb = cb;
    g_talk_user = user;
}

int zms_webrtc_server_has_talk_audio_cb(void)
{
    return g_talk_cb != NULL;
}

int zms_webrtc_server_emit_talk_audio(const char *app, const char *stream, int codec, int rate,
                                       const uint8_t *data, size_t len)
{
    if (!g_talk_cb) {
        return 0;
    }
    g_talk_cb(app, stream, codec, rate, data, len, g_talk_user);
    return 1;
}

size_t zms_webrtc_server_ice_servers(zms_webrtc_ice_server *out, size_t cap)
{
    zms_webrtc_server *srv = g_webrtc_service;
    size_t n;

    if (!srv || !out || cap == 0) {
        return 0;
    }
    n = srv->ice_server_count;
    if (n > cap) {
        n = cap;
    }
    if (n > 0) {
        memcpy(out, srv->ice_servers, n * sizeof(out[0]));
    }
    return n;
}

zms_webrtc_server *zms_webrtc_server_create(const zms_webrtc_server_opts *opts)
{
    zms_webrtc_server *srv;
    if (!opts || !opts->poller) {
        return NULL;
    }
    srv = (zms_webrtc_server *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller;
    srv->port_min = opts->port_min ? opts->port_min : 50000;
    srv->port_max = opts->port_max ? opts->port_max : 60000;
    if (opts->host && opts->host[0]) {
        strncpy(srv->host, opts->host, sizeof(srv->host) - 1);
    } else {
        strncpy(srv->host, "0.0.0.0", sizeof(srv->host) - 1);
    }
    if (opts->advertise_host && opts->advertise_host[0]) {
        strncpy(srv->advertise_host, opts->advertise_host, sizeof(srv->advertise_host) - 1);
    } else {
        strncpy(srv->advertise_host, "127.0.0.1", sizeof(srv->advertise_host) - 1);
    }
    if (opts->stun_server && opts->stun_server[0]) {
        zms_webrtc_ice_server *srvcfg = &srv->ice_servers[srv->ice_server_count++];
        snprintf(srvcfg->url, sizeof(srvcfg->url), "%s", opts->stun_server);
        srvcfg->turn = 0;
    }
    if (opts->turn_server && opts->turn_server[0]) {
        zms_webrtc_ice_server *srvcfg = &srv->ice_servers[srv->ice_server_count++];
        snprintf(srvcfg->url, sizeof(srvcfg->url), "%s", opts->turn_server);
        snprintf(srvcfg->user, sizeof(srvcfg->user), "%s",
                 opts->turn_user ? opts->turn_user : "");
        snprintf(srvcfg->pass, sizeof(srvcfg->pass), "%s",
                 opts->turn_pass ? opts->turn_pass : "");
        srvcfg->turn = 1;
    }
    int zice_on;
    (void)zms_webrtc_dtls_global_init();
    zms_webrtc_ice_port_init(opts->poller);
#if defined(ZMS_WEBRTC_USE_ZICE) && ZMS_WEBRTC_USE_ZICE
    zice_on = 1;
#else
    zice_on = 0;
#endif
    zn_info("[webrtc] play egress build=20250628 poller=http_session rtp_flush=%u zice=%d",
             (unsigned)ZMS_WEBRTC_PLAY_RTP_FLUSH, zice_on);
    g_webrtc_service = srv;
    if (opts->signaling_enable && opts->signaling_url && opts->signaling_url[0]) {
        if (zms_webrtc_signaling_start(opts->signaling_url, opts->signaling_room,
                                       opts->signaling_token, opts->signaling_app,
                                       opts->signaling_stream) != 0) {
            zn_warn("[webrtc] signaling client start failed url=%s", opts->signaling_url);
        }
    }
    client_autostart_add(srv->poller, "whep", opts->whep_client_url, opts->whep_client_app,
                         opts->whep_client_stream, opts->whep_client_token,
                         opts->client_ssl_ctx);
    client_autostart_add(srv->poller, "whip", opts->whip_client_url, opts->whip_client_app,
                         opts->whip_client_stream, opts->whip_client_token,
                         opts->client_ssl_ctx);
    return srv;
}

void zms_webrtc_server_destroy(zms_webrtc_server *srv)
{
    if (!srv) {
        return;
    }
    zms_webrtc_signaling_stop();
    zms_webrtc_client_shutdown();
    zms_webrtc_session_destroy_all();
    if (g_webrtc_service == srv) {
        g_webrtc_service = NULL;
    }
    zms_webrtc_ice_port_fini();
    free(srv);
    if (!g_webrtc_service) {
        zms_webrtc_dtls_global_fini();
    }
}
