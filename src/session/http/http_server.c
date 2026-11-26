/**
 * @file http_server.c
 * @brief HTTP/WS 服务端会话管理。
 */
#include "zms/play/vod_play_port.h"
#include "zms/session/http/http_server.h"
#include "zms/play/dispatch.h"
#include "zms/protocol/http/websocket/framer.h"
#include "zms/ops/config/config.h"
#include "zms/protocol/http/route.h"
#include "zms/protocol/http/session_api.h"
#include "session/http/session_internal.h"
#include "zms/protocol/http/request_reader.h"
#include "zms/play/play_binding.h"
#include "zms/core/event.h"
#include "zms/egress/mpegts/mpegts_egress.h"
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/ops/config/pull_ssl.h"
#include "zms/ops/config/netcheck.h"
#include "zms/media/media_limits.h"
#endif
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

struct zms_http_server *g_http_service_instance;

static zms_play_binding http_play_binding(zms_http_session *hs)
{
    zms_play_binding b;

    memset(&b, 0, sizeof(b));
    b.source = &hs->source;
    b.play = &hs->play;
    b.vod_lane = &hs->vod_lane;
    b.reader_attached = &hs->reader_attached;
    b.play_start_ms = &hs->play_start_ms;
    b.player = hs->play_event ? hs->play_event : "http-flv";
    return b;
}

static void http_play_teardown(zms_http_session *hs)
{
    zms_play_binding bind;

    if (!hs) {
        return;
    }
    /* Wire muxer 归协议所有；binding 仅关闭 readers/lane/记账。 */
    zms_flv_live_muxer_destroy(hs->live_muxer);
    zms_vod_flv_muxer_destroy(hs->vod_muxer);
    hs->live_muxer = NULL;
    hs->vod_muxer = NULL;
    if (hs->ts_muxer) {
        zms_mpegts_egress_destroy(hs->ts_muxer);
        hs->ts_muxer = NULL;
    }
    if (hs->play.readers.gop) {
        zms_session_dispatch_register_all();
        zms_session_detach_play(ZMS_SESSION_HTTP_TS, &hs->play);
    }
    bind = http_play_binding(hs);
    zms_play_binding_close(&bind, 1);
    hs->play_event = NULL;
    hs->ws_mode = 0;
}

static int http_ws_send_raw(void *user, const void *data, size_t len)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs || !hs->tcp) {
        return -1;
    }
    return zn_tcp_session_send(hs->tcp, data, len) == ZN_OK ? 0 : -1;
}

static void http_ws_on_close(void *user)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (hs) {
        zms_http_session_stop_stream(hs);
    }
}

void zms_http_session_stop_stream(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->tcp) {
        zn_tcp_session_out_discard(hs->tcp);
    }
    if (hs->file_fp) {
        zms_http_close_file_async(hs->file_fp, hs->poller);
        hs->file_fp = NULL;
    }
    free(hs->file_rd_buf);
    hs->file_rd_buf = NULL;
    hs->file_rd_len = 0;
    hs->file_rd_off = 0;
    hs->file_remain = 0;
    hs->hls_send_len = 0;
    hs->hls_send_off = 0;
    http_play_teardown(hs);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
    if (hs->splitter) {
        zms_http_request_reader_reset(hs->splitter);
    }
}

static int http_looks_like_request(const void *data, size_t len)
{
    const char *s;

    if (!data || len < 4) {
        return 0;
    }
    s = (const char *)data;
    return strncmp(s, "GET ", 4) == 0 || strncmp(s, "HEAD ", 5) == 0 ||
           strncmp(s, "POST ", 5) == 0 || strncmp(s, "PUT ", 4) == 0 ||
           strncmp(s, "OPTIONS ", 8) == 0;
}

int zms_http_session_authorize_play(zms_http_session *hs, const char *player,
                                    zms_media_source *src)
{
    zms_auth_request req;
    zms_auth_decision dec;
    char ip[64];
    unsigned port = 0;

    if (!hs || !src) {
        return 0;
    }
    zms_auth_fill_peer(hs->tcp, ip, sizeof(ip), &port);
    memset(&req, 0, sizeof(req));
    req.action = ZMS_AUTH_ACTION_PLAY;
    req.schema = player && player[0] ? player : src->schema;
    req.vhost = src->vhost[0] ? src->vhost : NULL;
    req.app = src->app;
    req.stream = src->stream;
    req.params = hs->params[0] ? hs->params : NULL;
    req.ip = ip[0] ? ip : NULL;
    req.port = port;
    if (zms_auth_check(&req, &dec)) {
        return 1;
    }
    zn_warn("[http] play denied schema=%s app=%s stream=%s", req.schema, req.app, req.stream);
    return 0;
}

int zms_http_session_send_reserve(zms_http_session *hs, size_t need)
{
    uint8_t *nb;

    if (!hs || need == 0) {
        return -1;
    }
    if (need <= hs->send_cap) {
        return 0;
    }
    if (need > ZMS_HTTP_SEND_BUF_MAX) {
        return -1;
    }
    nb = (uint8_t *)realloc(hs->send_buf, need);
    if (!nb) {
        return -1;
    }
    hs->send_buf = nb;
    hs->send_cap = need;
    return 0;
}

static void on_http_request(const zms_http_request *req, void *user)
{
    zms_http_session *hs = (zms_http_session *)user;

    if (!hs || !req) {
        return;
    }
    /* 本次请求的查询串（token 等）供鉴权验签。 */
    zms_auth_split_query(req->path, NULL, 0, hs->params, sizeof(hs->params));
    if (!zms_http_router_dispatch(hs, req)) {
        zms_http_response_send_error(hs, 404, "Not Found");
    }
}

static void on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs) {
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_WS_STREAMING) {
        (void)zms_ws_framer_consume_client((const uint8_t *)data, len, http_ws_send_raw,
                                           http_ws_on_close, hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_IDLE) {
        zms_http_request_reader_input(hs->splitter, data, len);
    } else if (http_looks_like_request(data, len)) {
        zms_http_session_stop_stream(hs);
        zms_http_request_reader_input(hs->splitter, data, len);
    }
    zms_http_session_flush(hs);
}

static void on_manager(zn_tcp_session *session, void *user)
{
    (void)session;
    zms_http_session_flush((zms_http_session *)user);
}

static void http_session_free(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->file_fp) {
        zms_http_close_file_async(hs->file_fp, hs->poller);
        hs->file_fp = NULL;
    }
    free(hs->file_rd_buf);
    hs->file_rd_buf = NULL;
    http_play_teardown(hs);
    zms_http_request_reader_destroy(hs->splitter);
    free(hs->send_buf);
    free(hs);
}

zn_tcp_session *zms_http_session_tcp(zms_http_session *hs)
{
    return hs ? hs->tcp : NULL;
}

zn_poller *zms_http_session_poller(zms_http_session *hs)
{
    if (!hs) {
        return NULL;
    }
    if (hs->tcp) {
        return zn_tcp_session_poller(hs->tcp);
    }
    return hs->poller;
}

zn_poller_pool *zms_http_session_poller_pool(zms_http_session *hs)
{
    return (hs && hs->server) ? hs->server->poller_pool : NULL;
}

const char *zms_http_session_api_secret(zms_http_session *hs)
{
    return (hs && hs->server) ? hs->server->api_secret : NULL;
}

const zms_config *zms_http_session_config(zms_http_session *hs)
{
    return (hs && hs->server) ? hs->server->cfg : NULL;
}

void zms_http_session_async_hold(zms_http_session *hs)
{
    if (hs) {
        ++hs->async_refs;
    }
}

void zms_http_session_async_release(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->async_refs > 0) {
        --hs->async_refs;
    }
    if (hs->closed && hs->async_refs == 0) {
        http_session_free(hs);
    }
}

int zms_http_session_is_live(const zms_http_session *hs)
{
    return hs && !hs->closed && hs->tcp != NULL;
}

static void on_error(zn_tcp_session *session, void *user)
{
    (void)session;
    zms_http_session *hs = (zms_http_session *)user;
    if (!hs) {
        return;
    }
    /* 会话关闭：清连接并打标记，真正释放等最后一个异步任务放手。 */
    zn_tcp_session_set_user(session, NULL);
    hs->tcp = NULL;
    hs->closed = 1;
    zms_http_session_async_release(hs);
}

static void *session_create_user(zn_tcp_server *srv, zn_tcp_session *session)
{
    (void)srv;
    zms_http_session *hs = (zms_http_session *)calloc(1, sizeof(*hs));
    if (!hs) {
        return NULL;
    }
    hs->server = g_http_service_instance;
    hs->send_cap = ZMS_HTTP_SEND_BUF_INIT;
    hs->send_buf = (uint8_t *)malloc(hs->send_cap);
    if (!hs->send_buf) {
        free(hs);
        return NULL;
    }
    hs->tcp = session;
    hs->poller = zn_tcp_session_poller(session);
    {
        zms_http_request_reader_opts opts = {on_http_request, hs};
        hs->splitter = zms_http_request_reader_create(&opts);
    }
    if (!hs->splitter) {
        free(hs->send_buf);
        free(hs);
        return NULL;
    }
    return hs;
}

zms_http_server *zms_http_server_create(const zms_http_server_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_http_server *srv = (zms_http_server *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    if (opts->api_secret && opts->api_secret[0]) {
        strncpy(srv->api_secret, opts->api_secret, sizeof(srv->api_secret) - 1);
    }
    srv->poller = opts->poller ? opts->poller : zn_poller_pool_get(opts->poller_pool, 0);
    srv->poller_pool = opts->poller_pool;
    srv->cfg = opts->cfg;
    g_http_service_instance = srv;

#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (opts->cfg && opts->cfg->webrtc.enable) {
        /* 并发上限必须在 server_create 之前落到会话注册表。 */
        zms_webrtc_session_set_limit(opts->cfg->webrtc.max_sessions);
        zms_webrtc_server_opts wopts = {
            .poller = srv->poller,
            .host = opts->host && opts->host[0] ? opts->host : "0.0.0.0",
            .advertise_host =
                opts->cfg->general.extern_ip[0] ? opts->cfg->general.extern_ip : "127.0.0.1",
            .port_min = (uint16_t)(opts->cfg->webrtc.port_min ? opts->cfg->webrtc.port_min : 50000),
            .port_max = (uint16_t)(opts->cfg->webrtc.port_max ? opts->cfg->webrtc.port_max : 60000),
            .stun_server = opts->cfg->webrtc.stun_server,
            .turn_server = opts->cfg->webrtc.turn_server,
            .turn_user = opts->cfg->webrtc.turn_user,
            .turn_pass = opts->cfg->webrtc.turn_pass,
            .signaling_enable = opts->cfg->webrtc.signaling_enable,
            .signaling_url = opts->cfg->webrtc.signaling_url,
            .signaling_room = opts->cfg->webrtc.signaling_room,
            .signaling_token = opts->cfg->webrtc.signaling_token,
            .signaling_app = opts->cfg->webrtc.signaling_app,
            .signaling_stream = opts->cfg->webrtc.signaling_stream,
            .whep_client_url = opts->cfg->webrtc.whep_client_url,
            .whep_client_app = opts->cfg->webrtc.whep_client_app,
            .whep_client_stream = opts->cfg->webrtc.whep_client_stream,
            .whep_client_token = opts->cfg->webrtc.whep_client_token,
            .whip_client_url = opts->cfg->webrtc.whip_client_url,
            .whip_client_app = opts->cfg->webrtc.whip_client_app,
            .whip_client_stream = opts->cfg->webrtc.whip_client_stream,
            .whip_client_token = opts->cfg->webrtc.whip_client_token,
            .client_ssl_ctx = zms_pull_ssl_ctx(opts->cfg),
        };
        srv->webrtc = zms_webrtc_server_create(&wopts);
        if (!srv->webrtc) {
            zn_warn("WebRTC service create failed");
        } else {
            const char *adv = zms_webrtc_server_advertise_host(srv->webrtc);

            zn_info("WebRTC ICE advertise_host=%s", adv);
            /* 回环地址只对"播放器与服务同机且走 loopback"有效；跨主机/浏览器
             * 观看时 ICE 连通性检查无法建立，表现为答案返回但无媒体。
             * 这里显式告警，避免把"[general] externIP 没填"误判为 WebRTC 故障。 */
            if (!adv || !adv[0] || strcmp(adv, "127.0.0.1") == 0 || strcmp(adv, "::1") == 0) {
                zn_warn("WebRTC ICE advertise_host is loopback (%s); remote viewers will not "
                        "connect. Set [general] externIP to this host's LAN/public IP.",
                        adv && adv[0] ? adv : "127.0.0.1");
            } else if (!zms_netcheck_host_ip_present(adv)) {
                /* 非回环但不是本机任何网卡：多半是换了网段/搬了机器后没改配置。
                 * 这种配置不会报错，只会让所有远端浏览器连不上。 */
                zn_warn("WebRTC: [general] externIP=%s is not an address on this host. "
                        "ICE candidates will point at a non-existent peer and browsers will "
                        "fail to connect. Update it to this host's real IP.",
                        adv);
            }
            zn_info("WebRTC max sessions=%u (hard cap %u)", (unsigned)zms_webrtc_session_limit(),
                     (unsigned)ZMS_WEBRTC_SESSION_MAX_HARD);
        }
    }
#endif

    zn_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    zn_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 8080,
        .backlog = 64,
        .poller_pool = opts->poller_pool,
        .session_ops = &ops,
        .session_create_user = session_create_user,
        .manager_interval_sec = 0.02f,
        .handshake_timeout_ms = opts->handshake_timeout_ms,
        .idle_timeout_ms = opts->idle_timeout_ms,
        .max_sessions = opts->max_sessions,
        .max_sessions_per_ip = opts->max_sessions_per_ip,
    };
    srv->tcp = zn_tcp_server_create(&topts);
    if (!srv->tcp) {
        free(srv);
        return NULL;
    }
    if (opts->ssl_ctx && opts->ssl_port) {
        zn_tcp_server_opts_t tlsopts = topts;
        tlsopts.port = opts->ssl_port;
        tlsopts.ssl_ctx = opts->ssl_ctx;
        srv->tcp_tls = zn_tcp_server_create(&tlsopts);
        if (!srv->tcp_tls) {
            zn_warn("HTTPS listen create failed port=%u", (unsigned)opts->ssl_port);
        } else {
            zn_info("HTTPS/WSS listen %s:%u", topts.host ? topts.host : "0.0.0.0",
                     (unsigned)opts->ssl_port);
        }
    }
    return srv;
}

void zms_http_server_destroy(zms_http_server *srv)
{
    if (!srv) {
        return;
    }
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (srv->webrtc) {
        zms_webrtc_server_destroy(srv->webrtc);
        srv->webrtc = NULL;
    }
#endif
    if (g_http_service_instance == srv) {
        g_http_service_instance = NULL;
    }
    zn_tcp_server_destroy(srv->tcp);
    zn_tcp_server_destroy(srv->tcp_tls);
    free(srv);
}

zn_err_t zms_http_server_start(zms_http_server *srv)
{
    zn_err_t err;

    if (!srv || !srv->tcp) {
        return ZN_ERR_INVALID;
    }
    err = zn_tcp_server_start(srv->tcp);
    if (err != ZN_OK) {
        return err;
    }
    if (srv->tcp_tls) {
        err = zn_tcp_server_start(srv->tcp_tls);
        if (err != ZN_OK) {
            zn_warn("HTTPS listen failed: %s", zn_strerror(err));
        }
    }
    return ZN_OK;
}

void zms_http_server_stop(zms_http_server *srv)
{
    if (srv && srv->tcp) {
        zn_tcp_server_stop(srv->tcp);
    }
    if (srv && srv->tcp_tls) {
        zn_tcp_server_stop(srv->tcp_tls);
    }
}

uint16_t zms_http_server_port(const zms_http_server *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return zn_tcp_server_port(srv->tcp);
}
