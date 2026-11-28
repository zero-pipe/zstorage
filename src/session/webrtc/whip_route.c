/**
 * @file whip_route.c
 * @brief WHIP（推流）路由。
 */
#include "zcs/session/webrtc/whep.h"
#include "zcs/protocol/http/route.h"
#include "zcs/protocol/http/session_api.h"
#include "session/webrtc/session_internal.h"
#include "zcs/session/webrtc/webrtc_server.h"
#include "zcs/ops/config/config.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int webrtc_http_enabled(const zcs_http_session *hs)
{
    if (!zcs_http_session_config(hs)) {
        return 1;
    }
    return zcs_http_session_config(hs)->webrtc.enable;
}

static void whip_copy_query_value(char *dst, size_t dst_sz, const char *val, size_t val_len)
{
    size_t n;

    if (!dst || dst_sz == 0) {
        return;
    }
    n = val_len < dst_sz - 1 ? val_len : dst_sz - 1;
    if (n > 0) {
        memcpy(dst, val, n);
    }
    dst[n] = '\0';
}

static int whip_query_key_is(const char *key, size_t key_len, const char *name)
{
    size_t n = strlen(name);

    return key_len == n && memcmp(key, name, n) == 0;
}

static int whip_parse_query(const char *path, char *app, char *stream, char *vhost)
{
    const char *q;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path) {
        return 0;
    }
    q = strchr(path, '?');
    if (!q) {
        return 0;
    }
    ++q;
    while (*q) {
        const char *amp = strchr(q, '&');
        const char *eq = strchr(q, '=');
        const char *val;
        size_t key_len;
        size_t val_len;

        if (!eq || (amp && eq >= amp)) {
            if (!amp) {
                break;
            }
            q = amp + 1;
            continue;
        }
        key_len = (size_t)(eq - q);
        val = eq + 1;
        val_len = amp ? (size_t)(amp - val) : strlen(val);
        if (whip_query_key_is(q, key_len, "vhost") && vhost) {
            whip_copy_query_value(vhost, ZCS_VHOST_MAX, val, val_len);
        } else if (whip_query_key_is(q, key_len, "app") && app) {
            whip_copy_query_value(app, ZCS_APP_MAX, val, val_len);
        } else if (whip_query_key_is(q, key_len, "stream") && stream) {
            whip_copy_query_value(stream, ZCS_STREAM_MAX, val, val_len);
        }
        if (!amp) {
            break;
        }
        q = amp + 1;
    }
    return app && app[0] && stream && stream[0];
}

static int webrtc_whip_route_match(const zcs_http_request *req)
{
    if (!req || !req->path[0]) {
        return 0;
    }
    return strncmp(req->path, "/index/api/whip", 15) == 0;
}

static int webrtc_api_path_id(const char *path, const char *api_prefix, char *id, size_t id_cap)
{
    const char *p;
    const char *q;
    size_t n;

    if (!path || !api_prefix || !id || id_cap == 0) {
        return 0;
    }
    n = strlen(api_prefix);
    if (strncmp(path, api_prefix, n) != 0) {
        return 0;
    }
    p = path + n;
    if (p[0] != '/' || !p[1]) {
        return 0;
    }
    ++p;
    q = strchr(p, '?');
    if (q) {
        size_t len = (size_t)(q - p);
        if (len == 0 || len >= id_cap) {
            return 0;
        }
        memcpy(id, p, len);
        id[len] = '\0';
        return id[0] != '\0';
    }
    if (strlen(p) >= id_cap) {
        return 0;
    }
    strcpy(id, p);
    return id[0] != '\0';
}

static void webrtc_whip_delete(zcs_http_session *hs, const zcs_http_request *req)
{
    char id[ZCS_WEBRTC_SESSION_ID_LEN];
    zcs_webrtc_session *sess;

    if (!hs || !req) {
        return;
    }
    if (!webrtc_http_enabled(hs)) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!webrtc_api_path_id(req->path, "/index/api/whip", id, sizeof(id))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    sess = zcs_webrtc_session_find(id);
    if (!sess || sess->mode != ZCS_WEBRTC_SESSION_PUBLISH) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zn_info("[webrtc] WHIP DELETE id=%s", id);
    zcs_webrtc_session_destroy(sess);
    zcs_http_response_send_bytes(hs, 200, "text/plain", "", 0);
}

static void webrtc_whip_patch(zcs_http_session *hs, const zcs_http_request *req)
{
    char id[ZCS_WEBRTC_SESSION_ID_LEN];
    zcs_webrtc_session *sess;

    if (!hs || !req) {
        return;
    }
    if (!webrtc_http_enabled(hs)) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!webrtc_api_path_id(req->path, "/index/api/whip", id, sizeof(id))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    sess = zcs_webrtc_session_find(id);
    if (!sess || sess->mode != ZCS_WEBRTC_SESSION_PUBLISH) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (!req->body || req->body_len == 0 ||
        zcs_webrtc_session_add_remote_candidates(sess, req->body, req->body_len) < 0) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    zcs_http_response_send_bytes(hs, 204, "text/plain", "", 0);
}

/* 鉴权通过后构建 SDP answer 并回复 201。 */
static void whip_send_answer(zcs_http_session *hs, zcs_webrtc_session *sess, const char *body,
                             size_t body_len)
{
    char answer[4096];
    char location[128];
    char hdr[512];
    size_t answer_len = 0;
    int hn;

    if (zcs_webrtc_session_build_publish_answer(sess, body, body_len, answer, sizeof(answer),
                                                &answer_len) != 0) {
        zcs_webrtc_session_destroy(sess);
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    snprintf(location, sizeof(location), "/index/api/whip/%s", sess->id);
    zn_info("[webrtc] WHIP answer app=%s stream=%s id=%s port=%u", sess->app, sess->stream, sess->id,
             (unsigned)sess->port);
    hn = snprintf(hdr, sizeof(hdr),
                  "HTTP/1.1 201 Created\r\n"
                  "Content-Type: application/sdp\r\n"
                  "Location: %s\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Access-Control-Expose-Headers: Location\r\n"
                  "Content-Length: %zu\r\n\r\n",
                  location, answer_len);
    zn_tcp_session_send(zcs_http_session_tcp(hs), hdr, (size_t)hn);
    zn_tcp_session_send(zcs_http_session_tcp(hs), answer, answer_len);
    zn_tcp_session_flush(zcs_http_session_tcp(hs));
}

static void webrtc_whip_post(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_webrtc_session *sess;
    char app[ZCS_APP_MAX];
    char vhost[ZCS_VHOST_MAX];
    char stream[ZCS_STREAM_MAX];
    zn_poller *pol;

    if (!hs || !req) {
        return;
    }
    if (!webrtc_http_enabled(hs)) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!zcs_webrtc_server_instance()) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    if (!whip_parse_query(req->path, app, stream, vhost) || !req->body || req->body_len == 0) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }

    pol = zcs_webrtc_server_poller(zcs_webrtc_server_instance());
    if (!pol) {
        pol = zcs_webrtc_server_resolve_poller(hs);
    }
    if (!pol) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }

    sess = zcs_webrtc_session_create_publish(vhost, app, stream, pol);
    if (!sess) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }

    /* 推流鉴权：上游签发的 token 在 URL 参数里，本地同步验签。 */
    {
        const char *q = strchr(req->path, '?');
        zcs_auth_request auth;
        zcs_auth_decision dec;
        char ip[64];
        unsigned port = 0;

        zcs_auth_fill_peer(zcs_http_session_tcp(hs), ip, sizeof(ip), &port);
        memset(&auth, 0, sizeof(auth));
        auth.action = ZCS_AUTH_ACTION_PUBLISH;
        auth.schema = "webrtc";
        auth.vhost = vhost;
        auth.app = app;
        auth.stream = stream;
        auth.params = q && q[1] ? q + 1 : NULL;
        auth.ip = ip[0] ? ip : NULL;
        auth.port = port;
        auth.id = sess->id;
        if (!zcs_auth_check(&auth, &dec)) {
            zn_warn("[webrtc] WHIP denied: app=%s stream=%s", sess->app, sess->stream);
            zcs_webrtc_session_destroy(sess);
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        if (sess->source) {
            sess->source->enable_mp4 = dec.enable_mp4;
        }
    }
    whip_send_answer(hs, sess, req->body, req->body_len);
}

void zcs_webrtc_whip_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    if (!hs || !req || !req->method[0]) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (strcmp(req->method, "OPTIONS") == 0) {
        zcs_http_response_send_bytes(hs, 204, "text/plain", "", 0);
        return;
    }
    if (strcmp(req->method, "POST") == 0) {
        webrtc_whip_post(hs, req);
        return;
    }
    if (strcmp(req->method, "DELETE") == 0) {
        webrtc_whip_delete(hs, req);
        return;
    }
    if (strcmp(req->method, "PATCH") == 0) {
        webrtc_whip_patch(hs, req);
        return;
    }
    zcs_http_response_send_error(hs, 405, "Method Not Allowed");
}

static void webrtc_whip_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_webrtc_whip_handle(hs, req);
}

static const zcs_http_route_ops k_webrtc_whip_route = {
    .name = "webrtc-whip",
    .match = webrtc_whip_route_match,
    .handle = webrtc_whip_route_handle,
};

void zcs_webrtc_whip_routes_register(void)
{
    zcs_http_route_register(&k_webrtc_whip_route);
}
