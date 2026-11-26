/**
 * @file rtmp_server.c
 * @brief RTMP 服务端监听：接受连接并创建 session。
 */
#include "zms/session/rtmp/rtmp_server.h"
#include "zms/session/rtmp/session.h"
#include "zms/play/dispatch.h"
#include "session/rtmp/session_internal.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>

struct zms_rtmp_server {
    zn_tcp_server *tcp;
    zn_tcp_server *tcp_tls;
    zn_poller *poller;
};

static void *session_create_user(zn_tcp_server *srv, zn_tcp_session *session)
{
    (void)srv;
    zms_rtmp_session_opts opts = {.tcp = session};
    return zms_rtmp_session_create(&opts);
}

static void on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (!s || s->destroy_scheduled) {
        return;
    }
    zms_rtmp_session_on_recv(s, data, len);
}

static void on_error(zn_tcp_session *session, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (s) {
        zms_rtmp_session_schedule_destroy(s, session);
    }
}

static void on_manager(zn_tcp_session *session, void *user)
{
    (void)session;
    zms_rtmp_session *s = (zms_rtmp_session *)user;
    if (!s || s->destroy_scheduled) {
        return;
    }
    zms_rtmp_session_on_manager(s);
}

zms_rtmp_server *zms_rtmp_server_create(const zms_rtmp_server_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_rtmp_server *srv = (zms_rtmp_server *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller ? opts->poller : zn_poller_pool_get(opts->poller_pool, 0);

    zn_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    zn_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 1935,
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
            zn_warn("RTMPS listen create failed port=%u", (unsigned)opts->ssl_port);
        } else {
            zn_info("RTMPS listen %s:%u", topts.host ? topts.host : "0.0.0.0",
                     (unsigned)opts->ssl_port);
        }
    }
    return srv;
}

void zms_rtmp_server_destroy(zms_rtmp_server *srv)
{
    if (!srv) {
        return;
    }
    zn_tcp_server_destroy(srv->tcp);
    zn_tcp_server_destroy(srv->tcp_tls);
    free(srv);
}

zn_err_t zms_rtmp_server_start(zms_rtmp_server *srv)
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
            zn_warn("RTMPS listen failed: %s", zn_strerror(err));
        }
    }
    return ZN_OK;
}

void zms_rtmp_server_stop(zms_rtmp_server *srv)
{
    if (srv && srv->tcp) {
        zn_tcp_server_stop(srv->tcp);
    }
    if (srv && srv->tcp_tls) {
        zn_tcp_server_stop(srv->tcp_tls);
    }
}

uint16_t zms_rtmp_server_port(const zms_rtmp_server *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return zn_tcp_server_port(srv->tcp);
}
