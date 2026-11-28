/**
 * @file whep_route.c
 * @brief WHEP（播放）路由。
 */
#include "zcs/session/webrtc/whep.h"
#include "zn/platform.h"
#include "zcs/protocol/http/route.h"
#include "zcs/protocol/http/session_api.h"
#include "session/webrtc/session_internal.h"
#include "zcs/core/auth.h"
#include "zcs/core/event.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/codec_filter.h"
#include "zcs/play/dispatch.h"
#include "zcs/session/webrtc/webrtc_server.h"
#include "zcs/ops/config/config.h"
#include "zcs/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif
static int webrtc_http_enabled(const zcs_http_session *hs)
{
    if (!zcs_http_session_config(hs)) {
        return 1;
    }
    return zcs_http_session_config(hs)->webrtc.enable;
}

static void whep_copy_query_value(char *dst, size_t dst_sz, const char *val, size_t val_len)
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

static int whep_query_key_is(const char *key, size_t key_len, const char *name)
{
    size_t n = strlen(name);
    return key_len == n && memcmp(key, name, n) == 0;
}

static int whep_parse_query(const char *path, char *app, char *stream, char *vhost, int *zlm_play)
{
    const char *q;
    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (zlm_play) {
        *zlm_play = 0;
    }
    if (!path) {
        return 0;
    }
    if (strstr(path, "/index/api/webrtc") != NULL && zlm_play) {
        *zlm_play = 1;
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
        if (whep_query_key_is(q, key_len, "vhost") && vhost) {
            whep_copy_query_value(vhost, ZCS_VHOST_MAX, val, val_len);
        } else if (whep_query_key_is(q, key_len, "app") && app) {
            whep_copy_query_value(app, ZCS_APP_MAX, val, val_len);
        } else if (whep_query_key_is(q, key_len, "stream") && stream) {
            whep_copy_query_value(stream, ZCS_STREAM_MAX, val, val_len);
        } else if (whep_query_key_is(q, key_len, "type") && zlm_play && val_len == 4 &&
                   memcmp(val, "play", 4) == 0) {
            *zlm_play = 1;
        }
        if (!amp) {
            break;
        }
        q = amp + 1;
    }
    return app && app[0] && stream && stream[0];
}

static int webrtc_whep_route_match(const zcs_http_request *req)
{
    if (!req || !req->path[0]) {
        return 0;
    }
    return strncmp(req->path, "/index/api/whep", 15) == 0 ||
           strncmp(req->path, "/index/api/webrtc", 17) == 0;
}

static void webrtc_send_sdp_response(zcs_http_session *hs, int status, const char *location,
                                     const char *sdp, size_t sdp_len)
{
    char hdr[512];
    int n;
    if (!hs || !zcs_http_session_tcp(hs) || !sdp) {
        return;
    }
    if (location && location[0]) {
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/sdp\r\n"
                     "Location: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Expose-Headers: Location\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 201 ? "Created" : "OK", location, sdp_len);
    } else {
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/sdp\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 201 ? "Created" : "OK", sdp_len);
    }
    zn_tcp_session_send(zcs_http_session_tcp(hs), hdr, (size_t)n);
    zn_tcp_session_send(zcs_http_session_tcp(hs), sdp, sdp_len);
    zn_tcp_session_flush(zcs_http_session_tcp(hs));
}

static void webrtc_send_zlm_json(zcs_http_session *hs, const char *sdp, size_t sdp_len,
                                 const char *whep_id)
{
    enum { WHEP_JSON_CAP = 32768u };
    uint8_t *body = NULL;
    size_t body_cap = 0;
    size_t i, j = 0;
    size_t tail;
    if (!hs || !sdp || sdp_len == 0) {
        return;
    }
    if (!zcs_buf_pool_slot_resize(&body, &body_cap, WHEP_JSON_CAP)) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    if (whep_id && whep_id[0]) {
        j += (size_t)snprintf((char *)body + j, body_cap - j, "{\"code\":0,\"id\":\"%s\",\"sdp\":\"",
                              whep_id);
    } else {
        j += (size_t)snprintf((char *)body + j, body_cap - j, "{\"code\":0,\"sdp\":\"");
    }
    for (i = 0; i < sdp_len; ++i) {
        if (sdp[i] == '\r') {
            continue;
        }
        if (j + 3 >= body_cap) {
            break;
        }
        if (sdp[i] == '\n') {
            body[j++] = '\\';
            body[j++] = 'n';
        } else if (sdp[i] == '\"') {
            body[j++] = '\\';
            body[j++] = '\"';
        } else if (sdp[i] == '\\') {
            body[j++] = '\\';
            body[j++] = '\\';
        } else {
            body[j++] = sdp[i];
        }
    }
    if (j + 3 >= body_cap) {
        zn_warn("[webrtc] ZLM JSON answer truncated sdp_len=%zu", sdp_len);
        zcs_buf_pool_slot_clear(&body, &body_cap);
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    tail = (size_t)snprintf((char *)body + j, body_cap - j, "\"}");
    if (tail >= body_cap - j) {
        zn_warn("[webrtc] ZLM JSON answer close truncated sdp_len=%zu", sdp_len);
        zcs_buf_pool_slot_clear(&body, &body_cap);
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    j += tail;
    zcs_http_response_send_json(hs, 200, (const char *)body, j);
    zcs_buf_pool_slot_clear(&body, &body_cap);
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

static void webrtc_whep_delete(zcs_http_session *hs, const zcs_http_request *req)
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
    if (!webrtc_api_path_id(req->path, "/index/api/whep", id, sizeof(id))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    sess = zcs_webrtc_session_find(id);
    if (!sess || sess->mode != ZCS_WEBRTC_SESSION_PLAY) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zn_info("[webrtc] WHEP DELETE id=%s", id);
    zcs_webrtc_session_destroy(sess);
    zcs_http_response_send_bytes(hs, 200, "text/plain", "", 0);
}

static void webrtc_whep_patch(zcs_http_session *hs, const zcs_http_request *req)
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
    if (!webrtc_api_path_id(req->path, "/index/api/whep", id, sizeof(id))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    sess = zcs_webrtc_session_find(id);
    if (!sess || sess->mode != ZCS_WEBRTC_SESSION_PLAY) {
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

static void webrtc_whep_post_impl(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    zcs_webrtc_session *sess;
    char app[ZCS_APP_MAX];
    char vhost[ZCS_VHOST_MAX];
    char stream[ZCS_STREAM_MAX];
    char answer[8192];
    char location[128];
    size_t answer_len = 0;
    int zlm_play = 0;
    zcs_session_play_opts pcfg;
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
    if (!whep_parse_query(req->path, app, stream, vhost, &zlm_play) || !req->body ||
        req->body_len == 0) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    if (strncmp(req->path, "/index/api/webrtc", 17) == 0 && !zlm_play) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, vhost, app, stream);
    if (!src || !src->gop_queue || zcs_media_source_is_vod(src) ||
        !zcs_media_source_use_gop_queue_play(src)) {
        zn_warn("[webrtc] play 404 app=%s stream=%s src=%p gop=%p vod=%d gop_play=%d", app, stream,
                 (void *)src, src ? (void *)src->gop_queue : NULL,
                 src ? zcs_media_source_is_vod(src) : 0,
                 src ? zcs_media_source_use_gop_queue_play(src) : 0);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zcs_session_capability_check_source(ZCS_PROTO_CAP_WEBRTC_PLAY, src) != ZN_OK) {
        zcs_session_capability_log_reject("webrtc", src, ZCS_PROTO_CAP_WEBRTC_PLAY);
        zcs_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }
    {
        const char *q = strchr(req->path, '?');
        zcs_auth_request auth;
        zcs_auth_decision dec;
        char ip[64];
        unsigned port = 0;

        zcs_auth_fill_peer(zcs_http_session_tcp(hs), ip, sizeof(ip), &port);
        memset(&auth, 0, sizeof(auth));
        auth.action = ZCS_AUTH_ACTION_PLAY;
        auth.schema = "webrtc";
        auth.vhost = vhost;
        auth.app = app;
        auth.stream = stream;
        auth.params = q && q[1] ? q + 1 : NULL;
        auth.ip = ip[0] ? ip : NULL;
        auth.port = port;
        if (!zcs_auth_check(&auth, &dec)) {
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    pol = zcs_webrtc_server_poller(zcs_webrtc_server_instance());
    if (!pol) {
        pol = zcs_webrtc_server_resolve_poller(hs);
    }
    if (!pol) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    sess = zcs_webrtc_session_create(src, app, stream, pol);
    if (!sess) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    zcs_session_dispatch_register_all();
    zcs_egress_source_init(&sess->play);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZCS_SESSION_WEBRTC;
    if (zcs_session_attach_play(ZCS_SESSION_WEBRTC, &sess->play, src, &pcfg) != ZN_OK) {
        zcs_webrtc_session_destroy(sess);
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    if (zcs_webrtc_session_build_answer(sess, req->body, req->body_len, answer, sizeof(answer),
                                        &answer_len) != 0) {
        zcs_session_detach_play(ZCS_SESSION_WEBRTC, &sess->play);
        zcs_webrtc_session_destroy(sess);
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    zcs_media_source_reader_add(src);
    sess->play_reader_attached = 1;
    sess->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, "webrtc");
    snprintf(location, sizeof(location), "/index/api/whep/%s", sess->id);
    zn_info("[webrtc] whep answer app=%s stream=%s id=%s port=%u", app, stream, sess->id,
             (unsigned)sess->port);
    if (zlm_play || strncmp(req->path, "/index/api/webrtc", 17) == 0) {
        webrtc_send_zlm_json(hs, answer, answer_len, sess->id);
    } else {
        webrtc_send_sdp_response(hs, 201, location, answer, answer_len);
    }
}

static void webrtc_whep_post(zcs_http_session *hs, const zcs_http_request *req)
{
#if defined(_WIN32)
    __try {
        webrtc_whep_post_impl(hs, req);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        zn_error("[webrtc] FAULT WHEP POST code=0x%08x path=%s", (unsigned)GetExceptionCode(),
                  req && req->path[0] ? req->path : "?");
        if (hs) {
            zcs_http_response_send_error(hs, 500, "Internal Server Error");
        }
    }
#else
    webrtc_whep_post_impl(hs, req);
#endif
}

void zcs_webrtc_whep_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    if (!hs || !req || !req->method[0]) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zn_debug("[webrtc] WHEP method=%s path=%s", req->method, req->path);
    if (strcmp(req->method, "OPTIONS") == 0) {
        zcs_http_response_send_bytes(hs, 204, "text/plain", "", 0);
        return;
    }
    if (strcmp(req->method, "POST") == 0) {
        webrtc_whep_post(hs, req);
        return;
    }
    if (strcmp(req->method, "DELETE") == 0) {
        webrtc_whep_delete(hs, req);
        return;
    }
    if (strcmp(req->method, "PATCH") == 0) {
        webrtc_whep_patch(hs, req);
        return;
    }
    zcs_http_response_send_error(hs, 405, "Method Not Allowed");
}

static void webrtc_whep_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_webrtc_whep_handle(hs, req);
}

static const zcs_http_route_ops k_webrtc_whep_route = {
    .name = "webrtc-whep",
    .match = webrtc_whep_route_match,
    .handle = webrtc_whep_route_handle,
};

void zcs_webrtc_whep_routes_register(void)
{
    zcs_http_route_register(&k_webrtc_whep_route);
}
