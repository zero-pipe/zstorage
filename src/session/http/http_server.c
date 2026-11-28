/**
 * @file http_server.c
 * @brief HTTP/WS 服务端会话管理。
 */
#include "zcs/play/vod_play_port.h"
#include "zcs/session/http/http_server.h"
#include "zcs/play/dispatch.h"
#include "zcs/protocol/http/websocket/framer.h"
#include "zcs/ops/config/config.h"
#include "zcs/protocol/http/route.h"
#include "zcs/protocol/http/session_api.h"
#include "session/http/session_internal.h"
#include "zcs/protocol/http/request_reader.h"
#include "zcs/play/play_binding.h"
#include "zcs/core/event.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

struct zcs_http_server *g_http_service_instance;

static zcs_play_binding http_play_binding(zcs_http_session *hs)
{
    zcs_play_binding b;

    memset(&b, 0, sizeof(b));
    b.source = &hs->source;
    b.play = &hs->play;
    b.vod_lane = &hs->vod_lane;
    b.reader_attached = &hs->reader_attached;
    b.play_start_ms = &hs->play_start_ms;
    b.player = hs->play_event ? hs->play_event : "http-flv";
    return b;
}

static void http_play_teardown(zcs_http_session *hs)
{
    zcs_play_binding bind;

    if (!hs) {
        return;
    }
    /* wire muxer 归协议所有；binding 仅关闭 readers/lane/记账。 */
    bind = http_play_binding(hs);
    zcs_play_binding_close(&bind, 1);
    hs->play_event = NULL;
    hs->ws_mode = 0;
}

static int http_ws_send_raw(void *user, const void *data, size_t len)
{
    zcs_http_session *hs = (zcs_http_session *)user;
    if (!hs || !hs->tcp) {
        return -1;
    }
    return zn_tcp_session_send(hs->tcp, data, len) == ZN_OK ? 0 : -1;
}

static void http_ws_on_close(void *user)
{
    zcs_http_session *hs = (zcs_http_session *)user;
    if (hs) {
        zcs_http_session_stop_stream(hs);
    }
}

void zcs_http_session_stop_stream(zcs_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->tcp) {
        zn_tcp_session_out_discard(hs->tcp);
    }
    if (hs->file_fp) {
        zcs_http_close_file_async(hs->file_fp, hs->poller);
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
    hs->state = ZCS_HTTP_SESSION_STATE_IDLE;
    if (hs->splitter) {
        zcs_http_request_reader_reset(hs->splitter);
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

int zcs_http_session_authorize_play(zcs_http_session *hs, const char *player,
                                    zcs_media_source *src)
{
    zcs_auth_request req;
    zcs_auth_decision dec;
    char ip[64];
    unsigned port = 0;

    if (!hs || !src) {
        return 0;
    }
    zcs_auth_fill_peer(hs->tcp, ip, sizeof(ip), &port);
    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PLAY;
    req.schema = player && player[0] ? player : src->schema;
    req.vhost = src->vhost[0] ? src->vhost : NULL;
    req.app = src->app;
    req.stream = src->stream;
    req.params = hs->params[0] ? hs->params : NULL;
    req.ip = ip[0] ? ip : NULL;
    req.port = port;
    if (zcs_auth_check(&req, &dec)) {
        return 1;
    }
    zn_warn("[http] play denied schema=%s app=%s stream=%s", req.schema, req.app, req.stream);
    return 0;
}

int zcs_http_session_send_reserve(zcs_http_session *hs, size_t need)
{
    uint8_t *nb;

    if (!hs || need == 0) {
        return -1;
    }
    if (need <= hs->send_cap) {
        return 0;
    }
    if (need > ZCS_HTTP_SEND_BUF_MAX) {
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

static void on_http_request(const zcs_http_request *req, void *user)
{
    zcs_http_session *hs = (zcs_http_session *)user;

    if (!hs || !req) {
        return;
    }
    /* 本次请求的查询串（token 等）供鉴权验签。 */
    zcs_auth_split_query(req->path, NULL, 0, hs->params, sizeof(hs->params));
    if (!zcs_http_router_dispatch(hs, req)) {
        zcs_http_response_send_error(hs, 404, "Not Found");
    }
}

static void on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    zcs_http_session *hs = (zcs_http_session *)user;
    if (!hs) {
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_WS_STREAMING) {
        (void)zcs_ws_framer_consume_client((const uint8_t *)data, len, http_ws_send_raw,
                                           http_ws_on_close, hs);
    } else if (hs->state == ZCS_HTTP_SESSION_STATE_IDLE) {
        zcs_http_request_reader_input(hs->splitter, data, len);
    } else if (http_looks_like_request(data, len)) {
        zcs_http_session_stop_stream(hs);
        zcs_http_request_reader_input(hs->splitter, data, len);
    }
    zcs_http_session_flush(hs);
}

static void on_manager(zn_tcp_session *session, void *user)
{
    (void)session;
    zcs_http_session_flush((zcs_http_session *)user);
}

static void http_session_free(zcs_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->file_fp) {
        zcs_http_close_file_async(hs->file_fp, hs->poller);
        hs->file_fp = NULL;
    }
    free(hs->file_rd_buf);
    hs->file_rd_buf = NULL;
    http_play_teardown(hs);
    zcs_http_request_reader_destroy(hs->splitter);
    free(hs->send_buf);
    free(hs);
}

zn_tcp_session *zcs_http_session_tcp(zcs_http_session *hs)
{
    return hs ? hs->tcp : NULL;
}

zn_poller *zcs_http_session_poller(zcs_http_session *hs)
{
    if (!hs) {
        return NULL;
    }
    if (hs->tcp) {
        return zn_tcp_session_poller(hs->tcp);
    }
    return hs->poller;
}

zn_poller_pool *zcs_http_session_poller_pool(zcs_http_session *hs)
{
    return (hs && hs->server) ? hs->server->poller_pool : NULL;
}

const char *zcs_http_session_api_secret(zcs_http_session *hs)
{
    return (hs && hs->server) ? hs->server->api_secret : NULL;
}

const zcs_config *zcs_http_session_config(zcs_http_session *hs)
{
    return (hs && hs->server) ? hs->server->cfg : NULL;
}

void zcs_http_session_async_hold(zcs_http_session *hs)
{
    if (hs) {
        ++hs->async_refs;
    }
}

void zcs_http_session_async_release(zcs_http_session *hs)
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

int zcs_http_session_is_live(const zcs_http_session *hs)
{
    return hs && !hs->closed && hs->tcp != NULL;
}

static void on_error(zn_tcp_session *session, void *user)
{
    (void)session;
    zcs_http_session *hs = (zcs_http_session *)user;
    if (!hs) {
        return;
    }
    /* 会话关闭：清连接并打标记，真正释放等最后一个异步任务放手。 */
    zn_tcp_session_set_user(session, NULL);
    hs->tcp = NULL;
    hs->closed = 1;
    zcs_http_session_async_release(hs);
}

static void *session_create_user(zn_tcp_server *srv, zn_tcp_session *session)
{
    (void)srv;
    zcs_http_session *hs = (zcs_http_session *)calloc(1, sizeof(*hs));
    if (!hs) {
        return NULL;
    }
    hs->server = g_http_service_instance;
    hs->send_cap = ZCS_HTTP_SEND_BUF_INIT;
    hs->send_buf = (uint8_t *)malloc(hs->send_cap);
    if (!hs->send_buf) {
        free(hs);
        return NULL;
    }
    hs->tcp = session;
    hs->poller = zn_tcp_session_poller(session);
    {
        zcs_http_request_reader_opts opts = {on_http_request, hs};
        hs->splitter = zcs_http_request_reader_create(&opts);
    }
    if (!hs->splitter) {
        free(hs->send_buf);
        free(hs);
        return NULL;
    }
    return hs;
}

zcs_http_server *zcs_http_server_create(const zcs_http_server_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zcs_session_dispatch_register_all();
    zcs_http_server *srv = (zcs_http_server *)calloc(1, sizeof(*srv));
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

void zcs_http_server_destroy(zcs_http_server *srv)
{
    if (!srv) {
        return;
    }
    if (g_http_service_instance == srv) {
        g_http_service_instance = NULL;
    }
    zn_tcp_server_destroy(srv->tcp);
    zn_tcp_server_destroy(srv->tcp_tls);
    free(srv);
}

zn_err_t zcs_http_server_start(zcs_http_server *srv)
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

void zcs_http_server_stop(zcs_http_server *srv)
{
    if (srv && srv->tcp) {
        zn_tcp_server_stop(srv->tcp);
    }
    if (srv && srv->tcp_tls) {
        zn_tcp_server_stop(srv->tcp_tls);
    }
}

uint16_t zcs_http_server_port(const zcs_http_server *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return zn_tcp_server_port(srv->tcp);
}
