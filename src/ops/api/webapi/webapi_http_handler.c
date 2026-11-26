/**
 * @file webapi_http_handler.c
 * @brief Web API 的 HTTP 路由注册。
 */
#include "zms/protocol/http/route.h"
#include "zms/protocol/http/session_api.h"
#include "zms/ops/api/webapi/webapi.h"
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "zn/net/socket.h"
#include "zn/net/tcp_server.h"

/**
 * webapi JSON 响应体上限（堆上分配）。
 * getMediaList 每路约 600B：1000 路要 600KB，8KB 会截断成非法 JSON。
 */
#define ZMS_WEBAPI_BODY_MAX (1024u * 1024u)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int webapi_route_match(const zms_http_request *req)
{
    if (!req || !req->path[0]) {
        return 0;
    }
    if (strncmp(req->path, "/index/api/webrtc", 17) == 0 ||
        strncmp(req->path, "/index/api/whep", 15) == 0 ||
        strncmp(req->path, "/index/api/whip", 15) == 0) {
        return 0;
    }
    return strncmp(req->path, "/index/api/", 11) == 0 || strcmp(req->path, "/index/") == 0 ||
           strcmp(req->path, "/index") == 0;
}

static void url_decode_inplace(char *s)
{
    char *w;

    if (!s) {
        return;
    }
    w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], '\0'};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static int query_get(const char *query, const char *key, char *out, size_t out_cap)
{
    char buf[1536];
    size_t key_len;

    if (!query || !key || !out || out_cap == 0) {
        return 0;
    }
    out[0] = '\0';
    key_len = strlen(key);
    strncpy(buf, query, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#ifdef _WIN32
    {
        char *ctx = NULL;
        for (char *tok = strtok_s(buf, "&", &ctx); tok; tok = strtok_s(NULL, "&", &ctx)) {
#else
    for (char *tok = strtok(buf, "&"); tok; tok = strtok(NULL, "&")) {
#endif
            char *eq = strchr(tok, '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            url_decode_inplace(tok);
            url_decode_inplace(eq + 1);
            if (strcmp(tok, key) == 0) {
                strncpy(out, eq + 1, out_cap - 1);
                out[out_cap - 1] = '\0';
                return 1;
            }
        }
#ifdef _WIN32
    }
#endif
    return 0;
}

static int webapi_peer_is_localhost(zms_http_session *hs)
{
    zn_tcp_session *tcp = zms_http_session_tcp(hs);
    zn_socket *sock;
    char ip[64];

    if (!tcp) {
        return 0;
    }
    sock = zn_tcp_session_socket(tcp);
    if (!sock || zn_socket_get_peer(sock, ip, sizeof(ip), NULL) != ZN_OK) {
        return 0;
    }
    return strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0;
}

static int webapi_check_secret(zms_http_session *hs, const char *req_secret)
{
    const char *cfg_secret = zms_http_session_api_secret(hs);

    /* 未配置 secret：只允许本机访问，避免 WebAPI 在 8080 对外裸奔。 */
    if (!cfg_secret || !cfg_secret[0]) {
        return webapi_peer_is_localhost(hs);
    }
    return req_secret && strcmp(req_secret, cfg_secret) == 0;
}

static int try_handle_download_file(zms_http_session *hs, const zms_http_request *req)
{
    char path_only[512];
    char query[1024];
    char secret[128];
    char file_path[ZMS_CFG_PATH_MAX];
    const char *qmark;
    size_t plen;

    if (!req || !req->path[0]) {
        return 0;
    }
    qmark = strchr(req->path, '?');
    if (qmark) {
        plen = (size_t)(qmark - req->path);
        if (plen >= sizeof(path_only)) {
            plen = sizeof(path_only) - 1;
        }
        memcpy(path_only, req->path, plen);
        path_only[plen] = '\0';
        strncpy(query, qmark + 1, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
    } else {
        strncpy(path_only, req->path, sizeof(path_only) - 1);
        path_only[sizeof(path_only) - 1] = '\0';
        query[0] = '\0';
    }
    if (strcmp(path_only, "/index/api/record/download") != 0) {
        return 0;
    }

    secret[0] = '\0';
    file_path[0] = '\0';
    (void)query_get(query, "secret", secret, sizeof(secret));
    (void)query_get(query, "file_path", file_path, sizeof(file_path));

    if (!webapi_check_secret(hs, secret)) {
        zms_http_response_send_json(hs, 200, "{\"code\":-100,\"msg\":\"auth failed\"}",
                                    strlen("{\"code\":-100,\"msg\":\"auth failed\"}"));
        return 1;
    }
    if (!file_path[0] || !zms_mp4_recorder_path_under_root(file_path)) {
        zms_http_response_send_json(
            hs, 200, "{\"code\":-300,\"msg\":\"invalid file_path\"}",
            strlen("{\"code\":-300,\"msg\":\"invalid file_path\"}"));
        return 1;
    }
    zms_http_response_send_download_file(hs, file_path);
    return 1;
}

static void webapi_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    /* 响应体与合并后的 query 都在堆上：poller 线程栈只留小缓冲。 */
    char *body = (char *)malloc(ZMS_WEBAPI_BODY_MAX);
    char path_buf[2048];
    const char *path_with_query;
    int http_status = 500;
    zn_poller *pol;
    zms_web_api_opts wopts;
    size_t body_len;

    if (!body) {
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    if (try_handle_download_file(hs, req)) {
        free(body);
        return;
    }

    path_with_query = req && req->path ? req->path : "";
    /* POST 表单参数合并进 query，兼容 ZLM/平台 mediakit Client.post */
    if (req && req->method && strcmp(req->method, "POST") == 0 && req->body && req->body_len > 0) {
        int has_q = strchr(path_with_query, '?') != NULL;
        int n = snprintf(path_buf, sizeof(path_buf), "%s%c%.*s", path_with_query, has_q ? '&' : '?',
                         (int)req->body_len, req->body);
        if (n > 0 && (size_t)n < sizeof(path_buf)) {
            path_with_query = path_buf;
        }
    }

    pol = zms_http_session_tcp(hs) ? zn_tcp_session_poller(zms_http_session_tcp(hs)) : NULL;
    if (!pol) {
        pol = zms_http_session_poller(hs);
    }
    wopts = (zms_web_api_opts){
        .api_secret = zms_http_session_api_secret(hs),
        .poller = pol,
        .poller_pool = zms_http_session_poller_pool(hs),
        .cfg = zms_http_session_config(hs),
    };
    body_len = zms_web_api_handle(zms_http_session_tcp(hs), req->method, path_with_query, &wopts, &http_status, body,
                                  ZMS_WEBAPI_BODY_MAX);
    if (body_len == 0 && http_status == 0) {
        free(body);
        return; /* 异步接口（如 ONVIF）已提交任务，稍后由 IO 线程回包 */
    }
    zms_http_response_send_json(hs, http_status, body, body_len);
    free(body);
}

static const zms_http_route_ops k_webapi_route = {
    .name = "webapi",
    .match = webapi_route_match,
    .handle = webapi_route_handle,
};

void zms_webapi_http_routes_register(void)
{
    zms_http_route_register(&k_webapi_route);
}
