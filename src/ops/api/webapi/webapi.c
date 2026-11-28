/**
 * @file webapi.c
 * @brief Web API 请求处理。
 */
#include "zcs/ops/api/webapi/webapi.h"
#include "ops/api/webapi/webapi_internal.h"
#include "zcs/pull/proxy.h"
#include "zcs/protocol/rtp/ps_server.h"
#include "zcs/ops/config/config.h"
#include "zcs/ops/config/pull_ssl.h"
#include "zcs/util/buf_pool.h"
#include "zcs/version.h"
#include "zcs/ops/api/json/media_json.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/event.h"
#include "zcs/vod/io/source.h"
#include "zcs/core/auth.h"
#include "zn/net/socket.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#endif
#endif
static void split_path_query(const char *path_with_query, char *path, size_t path_cap, char *query,
                             size_t query_cap)
{
    path[0] = query[0] = '\0';
    if (!path_with_query) {
        return;
    }
    const char *q = strchr(path_with_query, '?');
    if (q) {
        size_t plen = (size_t)(q - path_with_query);
        if (plen >= path_cap) {
            plen = path_cap - 1;
        }
        memcpy(path, path_with_query, plen);
        path[plen] = '\0';
        strncpy(query, q + 1, query_cap - 1);
    } else {
        strncpy(path, path_with_query, path_cap - 1);
    }
}

static void url_decode_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *w = s;
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
static void parse_query(const char *query, zcs_webapi_query_map *m)
{
    if (!query || !m) {
        return;
    }
    char buf[1024];

    zcs_auth_params_copy(query, m->raw_query, sizeof(m->raw_query));
    strncpy(buf, query, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#ifdef _WIN32
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
        char *key = tok;
        char *val = eq + 1;
        url_decode_inplace(key);
        url_decode_inplace(val);
        if (strcmp(key, "schema") == 0) {
            strncpy(m->schema, val, sizeof(m->schema) - 1);
        } else if (strcmp(key, "vhost") == 0) {
            strncpy(m->vhost, val, sizeof(m->vhost) - 1);
        } else if (strcmp(key, "app") == 0) {
            strncpy(m->app, val, sizeof(m->app) - 1);
        } else if (strcmp(key, "stream") == 0) {
            strncpy(m->stream, val, sizeof(m->stream) - 1);
        } else if (strcmp(key, "stream_id") == 0) {
            strncpy(m->stream_id, val, sizeof(m->stream_id) - 1);
        } else if (strcmp(key, "port") == 0) {
            strncpy(m->port, val, sizeof(m->port) - 1);
        } else if (strcmp(key, "secret") == 0) {
            strncpy(m->secret, val, sizeof(m->secret) - 1);
        } else if (strcmp(key, "force") == 0) {
            strncpy(m->force, val, sizeof(m->force) - 1);
        } else if (strcmp(key, "url") == 0) {
            strncpy(m->url, val, sizeof(m->url) - 1);
        } else if (strcmp(key, "kind") == 0 || strcmp(key, "type") == 0) {
            strncpy(m->kind, val, sizeof(m->kind) - 1);
        } else if (strcmp(key, "token") == 0) {
            strncpy(m->token, val, sizeof(m->token) - 1);
        } else if (strcmp(key, "tokens") == 0) {
            strncpy(m->tokens, val, sizeof(m->tokens) - 1);
        } else if (strcmp(key, "id") == 0) {
            strncpy(m->id, val, sizeof(m->id) - 1);
        } else if (strcmp(key, "key") == 0) {
            strncpy(m->key, val, sizeof(m->key) - 1);
        } else if (strcmp(key, "rtp_transport") == 0) {
            strncpy(m->rtp_transport, val, sizeof(m->rtp_transport) - 1);
        } else if (strcmp(key, "proxy_prefix") == 0) {
            strncpy(m->proxy_prefix, val, sizeof(m->proxy_prefix) - 1);
        } else if (strcmp(key, "pt") == 0) {
            strncpy(m->pt, val, sizeof(m->pt) - 1);
        } else if (strcmp(key, "ssrc") == 0) {
            strncpy(m->ssrc, val, sizeof(m->ssrc) - 1);
        } else if (strcmp(key, "tcp_mode") == 0) {
            strncpy(m->tcp_mode, val, sizeof(m->tcp_mode) - 1);
        } else if (strcmp(key, "dst_url") == 0) {
            strncpy(m->dst_url, val, sizeof(m->dst_url) - 1);
        } else if (strcmp(key, "dst_port") == 0) {
            strncpy(m->dst_port, val, sizeof(m->dst_port) - 1);
        } else if (strcmp(key, "file_path") == 0) {
            strncpy(m->file_path, val, sizeof(m->file_path) - 1);
        } else if (strcmp(key, "stamp") == 0) {
            strncpy(m->stamp, val, sizeof(m->stamp) - 1);
        } else if (strcmp(key, "speed") == 0) {
            strncpy(m->speed, val, sizeof(m->speed) - 1);
        } else if (strcmp(key, "channel") == 0) {
            strncpy(m->channel, val, sizeof(m->channel) - 1);
        } else if (strcmp(key, "start") == 0) {
            strncpy(m->start, val, sizeof(m->start) - 1);
        } else if (strcmp(key, "end") == 0) {
            strncpy(m->end, val, sizeof(m->end) - 1);
        } else if (strcmp(key, "slots") == 0) {
            strncpy(m->slots, val, sizeof(m->slots) - 1);
        } else if (strcmp(key, "slot") == 0) {
            strncpy(m->slot, val, sizeof(m->slot) - 1);
        }
    }
}
static int is_localhost_peer(zn_tcp_session *tcp)
{
    if (!tcp) {
        return 0;
    }
    zn_socket *sock = zn_tcp_session_socket(tcp);
    if (!sock) {
        return 0;
    }
    char ip[64];
    if (zn_socket_get_peer(sock, ip, sizeof(ip), NULL) != ZN_OK) {
        return 0;
    }
    return strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0;
}

static int check_secret(zn_tcp_session *tcp, const char *cfg_secret, const char *req_secret)
{
    /* 未配置 secret：只允许本机访问，避免 WebAPI 在端口对外时裸奔。 */
    if (!cfg_secret || !cfg_secret[0]) {
        return is_localhost_peer(tcp);
    }
    if (is_localhost_peer(tcp) && (!req_secret || !req_secret[0])) {
        return 1;
    }
    return req_secret && strcmp(req_secret, cfg_secret) == 0;
}
int webapi_jb_append(zcs_webapi_json_buf *jb, const char *fmt, ...)
{
    zcs_json_buf zjb = {jb->buf, jb->cap, jb->len};
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(zjb.buf + zjb.len, zjb.cap - zjb.len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= zjb.cap - zjb.len) {
        return -1;
    }
    jb->len = zjb.len + (size_t)n;
    return 0;
}
zcs_media_server_ports webapi_ports_from_cfg(const zcs_config *cfg)
{
    zcs_media_server_ports p = {0};
    if (cfg) {
        p.rtmp = cfg->rtmp.port;
        p.rtsp = cfg->rtsp.port;
        p.http = cfg->http.port;
        p.srt = cfg->srt.port;
    }
    return p;
}
static int append_media_json(zcs_media_source *s, zcs_webapi_json_buf *jb, int *first,
                             const zcs_media_server_ports *ports)
{
    zcs_json_buf zjb = {jb->buf, jb->cap, jb->len};
    int r = zcs_json_append_media_item(&zjb, s, first, ports);
    jb->len = zjb.len;
    return r;
}

typedef struct zcs_webapi_list_ctx {
    zcs_webapi_json_buf *jb;
    int first;
    zcs_media_server_ports ports;
} zcs_webapi_list_ctx;

static int list_visit(zcs_media_source *src, void *user)
{
    zcs_webapi_list_ctx *ctx = (zcs_webapi_list_ctx *)user;
    return append_media_json(src, ctx->jb, &ctx->first, &ctx->ports);
}

size_t webapi_respond_json(int code, const char *msg, const char *extra, char *body,
                           size_t body_cap)
{
    if (extra && extra[0]) {
        return (size_t)snprintf(body, body_cap, "{\"code\":%d,\"msg\":\"%s\",%s}", code, msg,
                                extra);
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":%d,\"msg\":\"%s\"}", code, msg);
}
static size_t handle_get_media_list(const zcs_webapi_query_map *q, zcs_webapi_json_buf *jb,
                                    const zcs_config *cfg)
{
    zcs_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zcs_webapi_list_ctx ctx = {jb, 1, webapi_ports_from_cfg(cfg)};
    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zcs_media_source_foreach(list_visit, &ctx, &filter);
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

static size_t handle_close_stream(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                  int *http_status)
{
    if (!q->app[0] || !q->stream[0]) {
        *http_status = 200;
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing app/stream", NULL, body, body_cap);
    }
    const char *schema = q->schema[0] ? q->schema : ZCS_SCHEMA_RTMP;
    zcs_media_source *src = zcs_media_source_find_api_ex(schema, q->vhost, q->app, q->stream);
    if (!src || !zcs_media_source_is_online(src)) {
        *http_status = 200;
        return webapi_respond_json(ZCS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
    }
    int force = q->force[0] && q->force[0] != '0';
    int ok = zcs_media_source_close(src, force);
    *http_status = 200;
    if (ok) {
        return webapi_respond_json(ZCS_API_SUCCESS, "success", "\"result\":0", body, body_cap);
    }
    return webapi_respond_json(ZCS_API_OTHER_FAILED, "close failed", "\"result\":-1", body, body_cap);
}

static size_t handle_get_media_info(const zcs_webapi_query_map *q, zcs_webapi_json_buf *jb,
                                    const zcs_config *cfg)
{
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return 0;
    }
    zcs_media_source *src = zcs_media_source_find_api_ex(q->schema, q->vhost, q->app, q->stream);
    if (!src || !zcs_media_source_is_online(src)) {
        return 0;
    }
    zcs_json_buf zjb = {jb->buf, jb->cap, jb->len};
    zcs_media_server_ports ports = webapi_ports_from_cfg(cfg);
    if (zcs_json_write_media_info(&zjb, src, &ports) != 0) {
        return 0;
    }
    jb->len = zjb.len;
    return jb->len;
}

typedef struct zcs_webapi_close_streams_ctx {
    const zcs_webapi_query_map *q;
    int count_hit;
    int count_closed;
    int force;
} zcs_webapi_close_streams_ctx;

static int close_streams_visit(zcs_media_source *src, void *user)
{
    zcs_webapi_close_streams_ctx *ctx = (zcs_webapi_close_streams_ctx *)user;
    ++ctx->count_hit;
    if (zcs_media_source_close(src, ctx->force)) {
        ++ctx->count_closed;
    }
    return 0;
}

static size_t handle_close_streams(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                   int *http_status)
{
    zcs_webapi_close_streams_ctx ctx = {q, 0, 0, q->force[0] && q->force[0] != '0'};
    zcs_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zcs_media_source_foreach(close_streams_visit, &ctx, &filter);
    *http_status = 200;
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"count_hit\":%d,\"count_closed\":%d}",
                            ctx.count_hit, ctx.count_closed);
}
static size_t handle_get_server_config(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":["
                            "[\"api.apiDebug\",\"0\"],"
                            "[\"general.mediaServerId\",\"zcs\"],"
                            "[\"protocol.enable_rtmp\",\"1\"],"
                            "[\"protocol.enable_rtsp\",\"1\"],"
                            "[\"protocol.enable_hls\",\"1\"]"
                            "]}");
}

static size_t handle_get_api_list(char *body, size_t body_cap)
{
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":["
                            "\"/index/api/system/api_list\","
                            "\"/index/api/system/version\","
                            "\"/index/api/system/health\","
                            "\"/index/api/system/bufpool\","
                            "\"/index/api/system/statistic\","
                            "\"/index/api/system/config\","
                            "\"/index/api/media/list\","
                            "\"/index/api/media/info\","
                            "\"/index/api/media/online\","
                            "\"/index/api/media/close\","
                            "\"/index/api/media/close_all\","
                            "\"/index/api/proxy/add\","
                            "\"/index/api/proxy/del\","
                            "\"/index/api/proxy/list\","
                            "\"/index/api/rtp/open\","
                            "\"/index/api/rtp/connect\","
                            "\"/index/api/rtp/close\","
                            "\"/index/api/rtp/list\","
                            "\"/index/api/record/load\","
                            "\"/index/api/record/download\","
                            "\"/index/api/record/delete\","
                            "\"/index/api/record/seek\","
                            "\"/index/api/record/speed\","
                            "\"/index/api/zstorage/record/start\","
                            "\"/index/api/zstorage/record/stop\","
                            "\"/index/api/zstorage/record/list\","
                            "\"/index/api/zstorage/record/query\","
                            "\"/index/api/zstorage/record/delete\","
                            "\"/index/api/zstorage/node/status\","
                            "\"/index/api/zstorage/playback/start\","
                            "\"/index/api/zstorage/playback/stop\""
                            "]}");
}
static size_t handle_is_media_online(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    *http_status = 200;
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing schema/app/stream", NULL, body,
                            body_cap);
    }
    zcs_media_source *src = zcs_media_source_find_api_ex(q->schema, q->vhost, q->app, q->stream);
    int online = src && zcs_media_source_is_online(src);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"online\":%s}",
                            online ? "true" : "false");
}

static size_t handle_version(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"version\":\"%s\"}}",
                            ZCS_VERSION_STRING);
}

size_t zcs_web_api_handle(zn_tcp_session *tcp, const char *method, const char *path_with_query,
                          const zcs_web_api_opts *opts, int *http_status, char *body,
                          size_t body_cap)
{
    if (!http_status || !body || body_cap < 64) {
        return 0;
    }
    *http_status = 500;
    body[0] = '\0';

    if (!method || (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)) {
        *http_status = 405;
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "method not allowed", NULL, body, body_cap);
    }

    char path[512];
    char query[1024];
    split_path_query(path_with_query, path, sizeof(path), query, sizeof(query));

    zcs_webapi_query_map q;
    memset(&q, 0, sizeof(q));
    parse_query(query, &q);

    /* 探活：不要求 secret，便于负载均衡 */
    if (strcmp(path, "/index/api/system/health") == 0) {
        return webapi_handle_health(body, body_cap, http_status);
    }

    const char *cfg_secret = opts ? opts->api_secret : NULL;
    if (!check_secret(tcp, cfg_secret, q.secret)) {
        *http_status = 200;
        return webapi_respond_json(ZCS_API_AUTH_FAILED, "auth failed", NULL, body, body_cap);
    }

    /* 接入 token 白名单同步：平台把启用中的 token 推到此端点，用于推拉流本地验签。 */
    if (strcmp(path, "/index/api/auth/tokens") == 0) {
        *http_status = 200;
        zcs_auth_set_tokens(q.tokens);
        return webapi_respond_json(0, "success", NULL, body, body_cap);
    }

    if (strcmp(path, "/index/api/system/api_list") == 0 || strcmp(path, "/index/") == 0 ||
        strcmp(path, "/index") == 0) {
        *http_status = 200;
        return handle_get_api_list(body, body_cap);
    }

    if (strcmp(path, "/index/api/media/list") == 0) {
        zcs_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_list(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/media/close") == 0) {
        return handle_close_stream(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/media/info") == 0) {
        zcs_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_info(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 200;
            return webapi_respond_json(ZCS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/media/close_all") == 0) {
        return handle_close_streams(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/media/online") == 0) {
        return handle_is_media_online(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/system/version") == 0) {
        return handle_version(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/system/bufpool") == 0) {
        return webapi_handle_buf_pool_stats(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/system/statistic") == 0) {
        return webapi_handle_statistic(body, body_cap, http_status);
    }


    if (strcmp(path, "/index/api/system/config") == 0) {
        return handle_get_server_config(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/proxy/add") == 0) {
        return webapi_handle_proxy_add(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/proxy/del") == 0) {
        return webapi_handle_proxy_del(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/proxy/list") == 0) {
        zcs_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_proxy_list(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/rtp/open") == 0) {
        return webapi_handle_rtp_open(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/rtp/connect") == 0) {
        return webapi_handle_rtp_connect(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/rtp/close") == 0) {
        return webapi_handle_rtp_close(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/rtp/list") == 0) {
        zcs_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_rtp_list(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/record/load") == 0) {
        return webapi_handle_load_mp4(&q, opts, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/record/delete") == 0) {
        return webapi_handle_delete_record(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/record/seek") == 0) {
        return webapi_handle_seek_record(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/record/speed") == 0) {
        return webapi_handle_record_speed(&q, body, body_cap, http_status);
    }

    /* downloadFile 由 HTTP 路由层直接流式发送文件 */
    if (strcmp(path, "/index/api/record/download") == 0) {
        *http_status = 200;
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "use http handler for download", NULL, body,
                            body_cap);
    }

    if (strcmp(path, "/index/api/zstorage/record/start") == 0) {
        return webapi_handle_zs_record_start(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/record/stop") == 0) {
        return webapi_handle_zs_record_stop(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/record/query") == 0) {
        return webapi_handle_zs_record_query(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/record/delete") == 0) {
        return webapi_handle_zs_record_delete(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/node/status") == 0) {
        return webapi_handle_zs_node_status(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/playback/start") == 0) {
        return webapi_handle_zs_playback_start(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/playback/stop") == 0) {
        return webapi_handle_zs_playback_stop(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/zstorage/record/list") == 0) {
        zcs_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_zs_record_list(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    *http_status = 404;
    return webapi_respond_json(ZCS_API_NOT_FOUND, "api not found", NULL, body, body_cap);
}