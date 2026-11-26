/**
 * @file webapi.c
 * @brief Web API 请求处理。
 */
#include "zms/ops/api/webapi/webapi.h"
#include "ops/api/webapi/webapi_internal.h"
#include "zms/pull/proxy.h"
#include "zms/protocol/rtp/ps_server.h"
#include "zms/ops/config/config.h"
#include "zms/ops/config/pull_ssl.h"
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
#include "zms/session/webrtc/webrtc_server.h"
#endif
#include "zms/util/buf_pool.h"
#include "zms/version.h"
#include "zms/ops/api/json/media_json.h"
#include "zms/core/stream/hub.h"
#include "zms/core/event.h"
#include "zms/device/onvif/onvif.h"
#include "zms/vod/io/source.h"
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "ops/api/webapi/onvif_api.h"
#include "ops/api/webapi/device_api.h"
#include "ops/api/webapi/gb28181_api.h"
#include "zms/core/auth.h"
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
static void parse_query(const char *query, zms_webapi_query_map *m)
{
    if (!query || !m) {
        return;
    }
    char buf[1024];

    zms_auth_params_copy(query, m->raw_query, sizeof(m->raw_query));
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
        } else if (strcmp(key, "addr") == 0) {
            strncpy(m->addr, val, sizeof(m->addr) - 1);
        } else if (strcmp(key, "user") == 0) {
            strncpy(m->user, val, sizeof(m->user) - 1);
        } else if (strcmp(key, "pass") == 0) {
            strncpy(m->pass, val, sizeof(m->pass) - 1);
        } else if (strcmp(key, "profile") == 0) {
            strncpy(m->profile, val, sizeof(m->profile) - 1);
        } else if (strcmp(key, "x") == 0) {
            strncpy(m->x, val, sizeof(m->x) - 1);
        } else if (strcmp(key, "y") == 0) {
            strncpy(m->y, val, sizeof(m->y) - 1);
        } else if (strcmp(key, "z") == 0) {
            strncpy(m->z, val, sizeof(m->z) - 1);
        } else if (strcmp(key, "stop") == 0) {
            strncpy(m->stop, val, sizeof(m->stop) - 1);
        } else if (strcmp(key, "timeout") == 0) {
            strncpy(m->timeout, val, sizeof(m->timeout) - 1);
        } else if (strcmp(key, "channel") == 0) {
            strncpy(m->channel, val, sizeof(m->channel) - 1);
        } else if (strcmp(key, "device") == 0) {
            strncpy(m->device, val, sizeof(m->device) - 1);
        } else if (strcmp(key, "cmd") == 0) {
            strncpy(m->cmd, val, sizeof(m->cmd) - 1);
        } else if (strcmp(key, "call_id") == 0) {
            strncpy(m->call_id, val, sizeof(m->call_id) - 1);
        } else if (strcmp(key, "tcp") == 0) {
            strncpy(m->tcp, val, sizeof(m->tcp) - 1);
        } else if (strcmp(key, "start") == 0) {
            strncpy(m->start, val, sizeof(m->start) - 1);
        } else if (strcmp(key, "end") == 0) {
            strncpy(m->end, val, sizeof(m->end) - 1);
        } else if (strcmp(key, "pan") == 0) {
            strncpy(m->pan, val, sizeof(m->pan) - 1);
        } else if (strcmp(key, "tilt") == 0) {
            strncpy(m->tilt, val, sizeof(m->tilt) - 1);
        } else if (strcmp(key, "zoom") == 0) {
            strncpy(m->zoom, val, sizeof(m->zoom) - 1);
        } else if (strcmp(key, "priority") == 0) {
            strncpy(m->priority, val, sizeof(m->priority) - 1);
        } else if (strcmp(key, "method") == 0) {
            strncpy(m->method, val, sizeof(m->method) - 1);
        } else if (strcmp(key, "description") == 0) {
            strncpy(m->description, val, sizeof(m->description) - 1);
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
int webapi_jb_append(zms_webapi_json_buf *jb, const char *fmt, ...)
{
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
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
zms_media_server_ports webapi_ports_from_cfg(const zms_config *cfg)
{
    zms_media_server_ports p = {0};
    if (cfg) {
        p.rtmp = cfg->rtmp.port;
        p.rtsp = cfg->rtsp.port;
        p.http = cfg->http.port;
        p.srt = cfg->srt.port;
    }
    return p;
}
static int append_media_json(zms_media_source *s, zms_webapi_json_buf *jb, int *first,
                             const zms_media_server_ports *ports)
{
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
    int r = zms_json_append_media_item(&zjb, s, first, ports);
    jb->len = zjb.len;
    return r;
}

typedef struct zms_webapi_list_ctx {
    zms_webapi_json_buf *jb;
    int first;
    zms_media_server_ports ports;
} zms_webapi_list_ctx;

static int list_visit(zms_media_source *src, void *user)
{
    zms_webapi_list_ctx *ctx = (zms_webapi_list_ctx *)user;
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
static size_t handle_get_media_list(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                    const zms_config *cfg)
{
    zms_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zms_webapi_list_ctx ctx = {jb, 1, webapi_ports_from_cfg(cfg)};
    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zms_media_source_foreach(list_visit, &ctx, &filter);
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

static size_t handle_close_stream(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                  int *http_status)
{
    if (!q->app[0] || !q->stream[0]) {
        *http_status = 200;
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing app/stream", NULL, body, body_cap);
    }
    const char *schema = q->schema[0] ? q->schema : ZMS_SCHEMA_RTMP;
    zms_media_source *src = zms_media_source_find_api_ex(schema, q->vhost, q->app, q->stream);
    if (!src || !zms_media_source_is_online(src)) {
        *http_status = 200;
        return webapi_respond_json(ZMS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
    }
    int force = q->force[0] && q->force[0] != '0';
    int ok = zms_media_source_close(src, force);
    *http_status = 200;
    if (ok) {
        return webapi_respond_json(ZMS_API_SUCCESS, "success", "\"result\":0", body, body_cap);
    }
    return webapi_respond_json(ZMS_API_OTHER_FAILED, "close failed", "\"result\":-1", body, body_cap);
}

static size_t handle_get_media_info(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                    const zms_config *cfg)
{
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return 0;
    }
    zms_media_source *src = zms_media_source_find_api_ex(q->schema, q->vhost, q->app, q->stream);
    if (!src || !zms_media_source_is_online(src)) {
        return 0;
    }
    zms_json_buf zjb = {jb->buf, jb->cap, jb->len};
    zms_media_server_ports ports = webapi_ports_from_cfg(cfg);
    if (zms_json_write_media_info(&zjb, src, &ports) != 0) {
        return 0;
    }
    jb->len = zjb.len;
    return jb->len;
}

typedef struct zms_webapi_close_streams_ctx {
    const zms_webapi_query_map *q;
    int count_hit;
    int count_closed;
    int force;
} zms_webapi_close_streams_ctx;

static int close_streams_visit(zms_media_source *src, void *user)
{
    zms_webapi_close_streams_ctx *ctx = (zms_webapi_close_streams_ctx *)user;
    ++ctx->count_hit;
    if (zms_media_source_close(src, ctx->force)) {
        ++ctx->count_closed;
    }
    return 0;
}

static size_t handle_close_streams(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                   int *http_status)
{
    zms_webapi_close_streams_ctx ctx = {q, 0, 0, q->force[0] && q->force[0] != '0'};
    zms_media_source_filter filter = {
        .schema = q->schema[0] ? q->schema : NULL,
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = q->app[0] ? q->app : NULL,
        .stream = q->stream[0] ? q->stream : NULL,
    };
    zms_media_source_foreach(close_streams_visit, &ctx, &filter);
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
                            "[\"general.mediaServerId\",\"zms\"],"
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
                            "\"/index/api/webrtc/stats\","
                            "\"/index/api/webrtc/capabilities\","
                            "\"/index/api/webrtc/client_start\","
                            "\"/index/api/webrtc/client_list\","
                            "\"/index/api/webrtc/client_stop\","
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
                            "\"/index/api/onvif/discover\","
                            "\"/index/api/onvif/probe\","
                            "\"/index/api/onvif/add\","
                            "\"/index/api/onvif/del\","
                            "\"/index/api/onvif/list\","
                            "\"/index/api/onvif/ptz\","
                            "\"/index/api/gb28181/status\","
                            "\"/index/api/gb28181/channels\","
                            "\"/index/api/gb28181/alarm\","
                            "\"/index/api/device/capabilities\","
                            "\"/index/api/gb28181/platform/devices\","
                            "\"/index/api/gb28181/platform/channels\","
                            "\"/index/api/gb28181/platform/query\","
                            "\"/index/api/gb28181/platform/invite\","
                            "\"/index/api/gb28181/platform/bye\","
                            "\"/index/api/gb28181/platform/sessions\","
                            "\"/index/api/gb28181/platform/ptz\","
                            "\"/index/api/gb28181/platform/record_query\","
                            "\"/index/api/gb28181/platform/records\","
                            "\"/index/api/gb28181/platform/alarms\""
                            "]}");
}
static size_t handle_is_media_online(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    *http_status = 200;
    if (!q->schema[0] || !q->app[0] || !q->stream[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing schema/app/stream", NULL, body,
                            body_cap);
    }
    zms_media_source *src = zms_media_source_find_api_ex(q->schema, q->vhost, q->app, q->stream);
    int online = src && zms_media_source_is_online(src);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"online\":%s}",
                            online ? "true" : "false");
}

static size_t handle_version(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"version\":\"%s\"}}",
                            ZMS_VERSION_STRING);
}

/* 由 webapi_webrtc.c 提供（按 ZMS_ENABLE_WEBRTC 条件编译） */
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
size_t webapi_handle_webrtc_stats(char *body, size_t body_cap, int *http_status);
size_t webapi_handle_webrtc_capabilities(char *body, size_t body_cap, int *http_status);
size_t webapi_handle_start_webrtc_client(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                         const zms_config *cfg);
size_t webapi_handle_get_webrtc_clients(zms_webapi_json_buf *jb);
size_t webapi_handle_stop_webrtc_client(const zms_webapi_query_map *q, zms_webapi_json_buf *jb);
#endif

size_t zms_web_api_handle(zn_tcp_session *tcp, const char *method, const char *path_with_query,
                          const zms_web_api_opts *opts, int *http_status, char *body,
                          size_t body_cap)
{
    if (!http_status || !body || body_cap < 64) {
        return 0;
    }
    *http_status = 500;
    body[0] = '\0';

    if (!method || (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)) {
        *http_status = 405;
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "method not allowed", NULL, body, body_cap);
    }

    char path[512];
    char query[1024];
    split_path_query(path_with_query, path, sizeof(path), query, sizeof(query));

    zms_webapi_query_map q;
    memset(&q, 0, sizeof(q));
    parse_query(query, &q);

    /* 探活：不要求 secret，便于负载均衡 */
    if (strcmp(path, "/index/api/system/health") == 0) {
        return webapi_handle_health(body, body_cap, http_status);
    }

    const char *cfg_secret = opts ? opts->api_secret : NULL;
    if (!check_secret(tcp, cfg_secret, q.secret)) {
        *http_status = 200;
        return webapi_respond_json(ZMS_API_AUTH_FAILED, "auth failed", NULL, body, body_cap);
    }

    /* 接入 token 白名单同步：平台把启用中的 token 推到此端点，用于推拉流本地验签。 */
    if (strcmp(path, "/index/api/auth/tokens") == 0) {
        *http_status = 200;
        zms_auth_set_tokens(q.tokens);
        return webapi_respond_json(0, "success", NULL, body, body_cap);
    }

    if (strcmp(path, "/index/api/device/capabilities") == 0) {
        *http_status = 200;
        return (size_t)zms_webapi_device_capabilities(body, body_cap);
    }
    if (strcmp(path, "/index/api/device/events") == 0) {
        *http_status = 200;
        return (size_t)zms_webapi_device_events(body, body_cap);
    }
    if (strcmp(path, "/index/api/device/ptz") == 0) {
        *http_status = 200;
        return (size_t)zms_webapi_device_ptz(q.channel, q.pan[0] ? (float)atof(q.pan) : 0.0f,
                                             q.tilt[0] ? (float)atof(q.tilt) : 0.0f,
                                             q.zoom[0] ? (float)atof(q.zoom) : 0.0f,
                                             q.stop[0] && q.stop[0] != '0', body, body_cap);
    }

    /* GB28181 设备端：状态 / 通道 / 报警上报 */
    if (strncmp(path, "/index/api/gb28181/", 19) == 0) {
        zms_gb28181_api_req greq;
        int rc;
        memset(&greq, 0, sizeof(greq));
        greq.endpoint = path + 19;
        greq.channel = q.channel;
        greq.device = q.device;
        greq.cmd = q.cmd;
        greq.call_id = q.call_id;
        greq.app = q.app;
        greq.stream = q.stream;
        greq.start = q.start;
        greq.end = q.end;
        greq.pan = q.pan[0] ? (float)atof(q.pan) : 0.0f;
        greq.tilt = q.tilt[0] ? (float)atof(q.tilt) : 0.0f;
        greq.zoom = q.zoom[0] ? (float)atof(q.zoom) : 0.0f;
        greq.stop = q.stop[0] && q.stop[0] != '0';
        greq.tcp = q.tcp[0] ? atoi(q.tcp) : 0;
        greq.priority = q.priority[0] ? atoi(q.priority) : 0;
        greq.method = q.method[0] ? atoi(q.method) : 0;
        greq.description = q.description;
        rc = zms_webapi_gb28181_handle(&greq, http_status, body, body_cap);
        if (rc > 0) {
            return (size_t)rc;
        }
        *http_status = 404;
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "unknown gb28181 endpoint", NULL, body,
                            body_cap);
    }

    /* 设备接入（ONVIF）：独立命名空间；网络调用异步执行，回包在 IO 线程完成 */
    if (strncmp(path, "/index/api/onvif/", 17) == 0) {
        zms_onvif_api_req oreq;
        int rc;
        memset(&oreq, 0, sizeof(oreq));
        oreq.endpoint = path + 17;
        oreq.addr = q.addr;
        oreq.user = q.user;
        oreq.pass = q.pass;
        oreq.profile = q.profile;
        oreq.vhost = q.vhost;
        oreq.app = q.app;
        oreq.stream = q.stream;
        oreq.key = q.key;
        oreq.timeout_ms = q.timeout[0] ? atoi(q.timeout) : 0;
        oreq.x = q.x[0] ? (float)atof(q.x) : 0.0f;
        oreq.y = q.y[0] ? (float)atof(q.y) : 0.0f;
        oreq.z = q.z[0] ? (float)atof(q.z) : 0.0f;
        oreq.stop = q.stop[0] && q.stop[0] != '0';
        rc = zms_webapi_onvif_handle(tcp, opts, &oreq, http_status, body, body_cap);
        if (rc == 0) {
            return 0; /* 异步：on_io 已负责回包 */
        }
        if (rc > 0) {
            return strlen(body);
        }
        *http_status = 404;
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "unknown onvif endpoint", NULL, body, body_cap);
    }

    if (strcmp(path, "/index/api/system/api_list") == 0 || strcmp(path, "/index/") == 0 ||
        strcmp(path, "/index") == 0) {
        *http_status = 200;
        return handle_get_api_list(body, body_cap);
    }

    if (strcmp(path, "/index/api/media/list") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_list(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/media/close") == 0) {
        return handle_close_stream(&q, body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/media/info") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = handle_get_media_info(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 200;
            return webapi_respond_json(ZMS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
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

#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    if (strcmp(path, "/index/api/webrtc/stats") == 0) {
        return webapi_handle_webrtc_stats(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/webrtc/capabilities") == 0) {
        return webapi_handle_webrtc_capabilities(body, body_cap, http_status);
    }

    if (strcmp(path, "/index/api/webrtc/client_start") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_start_webrtc_client(&q, &jb, opts ? opts->cfg : NULL);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/webrtc/client_list") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_get_webrtc_clients(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

    if (strcmp(path, "/index/api/webrtc/client_stop") == 0) {
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_stop_webrtc_client(&q, &jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
        }
        *http_status = 200;
        return n;
    }

#endif /* ZMS_ENABLE_WEBRTC */

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
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_proxy_list(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
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
        zms_webapi_json_buf jb = {body, body_cap, 0};
        size_t n = webapi_handle_rtp_list(&jb);
        if (n == 0) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
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
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "use http handler for download", NULL, body,
                            body_cap);
    }

    *http_status = 404;
    return webapi_respond_json(ZMS_API_NOT_FOUND, "api not found", NULL, body, body_cap);
}