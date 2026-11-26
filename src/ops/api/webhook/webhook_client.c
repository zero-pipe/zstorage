/**
 * @file webhook_client.c
 * @brief webhook 事件通知：非阻塞 HTTP POST，全部在主 poller 上完成。
 *
 * 线程模型（见 docs/stream-pipeline.md）：
 *   - 任何线程都可以调用 zms_webhook_on_*：消息先打包成堆上任务，
 *     用 zn_poller_async 投递到主 poller；
 *   - 主 poller 上用 zn_tcp_client 做非阻塞连接/收发，超时用 zn_poller_do_delay；
 *   - 不做 DNS：所有钩子主机名在 init 阶段解析一次并缓存为 IP；
 *   - 不使用线程池、不调用阻塞 socket API。
 */
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/ops/api/json/media_json.h"
#include "zms/core/stream/hub.h"
#include "zms/version.h"
#include "zn/net/tcp_client.h"
#include "zn/platform.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/** 通知 JSON 体上限（堆上分配，不占栈）。 */
#define ZMS_HOOK_BODY_MAX 8192
/** 通知响应上限：回调只需判断 code/close，1KB 足够。 */
#define ZMS_HOOK_RESP_MAX 1024
/** 钩子 URL 主机数上限（通常全部指向同一个后台）。 */
#define ZMS_HOOK_HOST_MAX 8

static zn_poller *g_poller;
static zms_hook_config g_hook;
static int g_webhook_initialized;
static uint64_t g_hook_index;
static char g_play_host[64];
static unsigned g_play_http_port;

/* 心跳/续注册：保存注册所需的配置与端口，定时重发完整注册体。 */
static const zms_config *g_kp_cfg;
static zms_media_server_ports g_kp_ports;
static int g_kp_enabled;
static zn_poller_timer *g_kp_timer;

/** init 阶段解析好的「主机名 → IP」，poller 上只按表连接。 */
typedef struct zms_hook_host {
    char host[256];
    char ip[64];
} zms_hook_host;

static zms_hook_host g_hosts[ZMS_HOOK_HOST_MAX];
static int g_host_count;

/* ── JSON 拼装 ───────────────────────────────────────────────────────────── */

static int json_append_common(char *body, size_t cap, size_t *len, const char *prefix)
{
    uint64_t idx = ++g_hook_index;
    int n = snprintf(body + *len, cap - *len, "%s\"mediaServerId\":\"%s\",\"hook_index\":%llu",
                     prefix ? prefix : "", g_hook.media_server_id, (unsigned long long)idx);
    if (n < 0 || (size_t)n >= cap - *len) {
        return -1;
    }
    *len += (size_t)n;
    return 0;
}

static int schema_allowed(const char *schema)
{
    char buf[ZMS_CFG_SCHEMA_FILTER_MAX];

    if (!schema || !schema[0]) {
        return 0;
    }
    if (!g_hook.stream_changed_schemas[0]) {
        return 1;
    }
    strncpy(buf, g_hook.stream_changed_schemas, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#ifdef _WIN32
    {
        char *ctx = NULL;
        for (char *tok = strtok_s(buf, "/", &ctx); tok; tok = strtok_s(NULL, "/", &ctx)) {
#else
    {
        for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
#endif
            while (*tok == ' ' || *tok == '\t') {
                ++tok;
            }
            if (strcmp(tok, schema) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * on_stream_changed 体。regist=1 时内嵌完整流信息（与 webapi 同构），
 * 需要一块临时缓冲拼 JSON，因此从堆上取，避免 poller 线程 4KB 以上栈帧。
 */
static int build_stream_changed_regist(zms_media_source *src, int regist, char *body, size_t cap)
{
    size_t len = 0;
    int n;

    if (!src || !body) {
        return -1;
    }
    if (regist) {
        char *item = (char *)malloc(ZMS_HOOK_BODY_MAX);
        zms_json_buf jb;
        int first = 1;

        if (!item) {
            return -1;
        }
        jb.buf = item;
        jb.cap = ZMS_HOOK_BODY_MAX;
        jb.len = 0;
        if (zms_json_append_media_item(&jb, src, &first, NULL) != 0 || jb.len < 2) {
            free(item);
            return -1;
        }
        /* item 形如 {...}：去掉首 '{' 由外层补，去掉尾 '}' 由外层补。 */
        n = snprintf(body, cap, "{%.*s", (int)(jb.len - 2), item + 1);
        free(item);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        len = (size_t)n;
    } else {
        n = snprintf(body, cap,
                     "{\"schema\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\"",
                     src->schema, src->vhost, src->app, src->stream);
        if (n < 0 || (size_t)n >= cap) {
            return -1;
        }
        len = (size_t)n;
    }
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    n = snprintf(body + len, cap - len, ",\"regist\":%s}", regist ? "true" : "false");
    if (n < 0 || (size_t)n >= cap - len) {
        return -1;
    }
    return 0;
}

static int build_tuple_body(const zms_media_tuple *t, const char *player_schema, char *body,
                            size_t cap)
{
    const char *proto;
    size_t len;
    int n;

    if (!t || !body) {
        return -1;
    }
    proto = player_schema && player_schema[0] ? player_schema : t->schema;
    n = snprintf(body, cap,
                 "{\"schema\":\"%s\",\"protocol\":\"%s\",\"vhost\":\"%s\",\"app\":\"%s\","
                 "\"stream\":\"%s\",\"params\":\"\"",
                 t->schema, proto, t->vhost, t->app, t->stream);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    len = (size_t)n;
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    n = snprintf(body + len, cap - len, "}");
    if (n < 0 || (size_t)n >= cap - len) {
        return -1;
    }
    return 0;
}

/* ── 非阻塞 HTTP POST ────────────────────────────────────────────────────── */

typedef struct zms_hook_req zms_hook_req;

struct zms_hook_req {
    zn_poller *io;
    zn_tcp_client *cli;
    zn_poller_timer *timer;
    int awaiting_response;
    int retry_left;
    char url[ZMS_CFG_HOOK_URL_MAX];
    char host[256];
    char path[512];
    uint16_t port;
    char *req;
    size_t req_len;
    char resp[ZMS_HOOK_RESP_MAX];
    size_t resp_len;
    zms_media_tuple tuple;
    int has_tuple;
    int none_reader;
    zms_hook_req *next;
};

static zms_hook_req *g_reqs;
static int32_t g_hook_pending;

typedef struct zms_hook_notify {
    char url[ZMS_CFG_HOOK_URL_MAX];
    char *body;
    zms_media_tuple tuple;
    int has_tuple;
    int none_reader;
} zms_hook_notify;

static int parse_http_url(const char *url, char *host, size_t host_cap, uint16_t *port,
                          char *path, size_t path_cap)
{
    const char *p;
    const char *slash;
    char *colon;
    size_t host_len;

    if (!url || strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    p = url + 7;
    slash = strchr(p, '/');
    if (slash) {
        host_len = (size_t)(slash - p);
        snprintf(path, path_cap, "%s", slash);
    } else {
        host_len = strlen(p);
        snprintf(path, path_cap, "/");
    }
    if (host_len == 0 || host_len >= host_cap) {
        return -1;
    }
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    *port = 80;
    colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
        if (*port == 0) {
            *port = 80;
        }
    }
    return 0;
}

/** 查 init 阶段的解析结果；找不到返回 0（poller 上不允许做 DNS）。 */
static int host_ip_lookup(const char *host, char *out, size_t cap)
{
    for (int i = 0; i < g_host_count; ++i) {
        if (strcmp(g_hosts[i].host, host) == 0 && g_hosts[i].ip[0]) {
            snprintf(out, cap, "%s", g_hosts[i].ip);
            return 1;
        }
    }
    return 0;
}

static void req_unlink(zms_hook_req *r)
{
    zms_hook_req **pp = &g_reqs;

    while (*pp) {
        if (*pp == r) {
            *pp = r->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void req_free(zms_hook_req *r)
{
    if (!r) {
        return;
    }
    if (r->timer) {
        zn_poller_timer_cancel(r->timer);
        r->timer = NULL;
    }
    if (r->cli) {
        zn_tcp_client_destroy(r->cli);
        r->cli = NULL;
    }
    free(r->req);
    req_unlink(r);
    free(r);
    if (g_hook_pending > 0) {
        --g_hook_pending;
    }
}

static int resp_close_requested(const char *resp)
{
    const char *p;
    const char *body;

    if (!resp) {
        return 0;
    }
    body = strstr(resp, "\r\n\r\n");
    body = body ? body + 4 : resp;
    p = strstr(body, "\"close\"");
    if (!p) {
        return 0;
    }
    p = strchr(p, ':');
    if (!p) {
        return 0;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return strncmp(p, "true", 4) == 0;
}

/* 通知收尾：on_none_reader 的 close:true 在这里关流，然后在 poller 线程释放。 */
static void req_finish(zms_hook_req *r, int err)
{
    if (err != 0) {
        zn_warn("webhook POST failed: %s", r->url);
    } else if (r->none_reader && g_webhook_initialized && resp_close_requested(r->resp)) {
        zms_media_source *src = zms_media_source_find(r->tuple.schema, r->tuple.app,
                                                      r->tuple.stream);

        if (!src) {
            src = zms_media_source_find_api(r->tuple.schema, r->tuple.app, r->tuple.stream);
        }
        if (src) {
            zn_warn("webhook close stream on none_reader: %s/%s", r->tuple.app, r->tuple.stream);
            zms_media_source_close(src, 0);
        }
    }
    req_free(r);
}

static void req_start(zms_hook_req *r);

static uint64_t req_retry_cb(void *user)
{
    zms_hook_req *r = (zms_hook_req *)user;

    r->timer = NULL;
    req_start(r);
    return 0;
}

static void req_fail(zms_hook_req *r, const char *why)
{
    if (r->timer) {
        zn_poller_timer_cancel(r->timer);
        r->timer = NULL;
    }
    if (r->cli) {
        zn_tcp_client_destroy(r->cli);
        r->cli = NULL;
    }
    if (r->retry_left > 0) {
        int delay = (int)(g_hook.retry_delay_sec * 1000.f);

        if (delay < 0) {
            delay = 0;
        }
        --r->retry_left;
        r->awaiting_response = 0;
        r->resp_len = 0;
        r->resp[0] = '\0';
        zn_warn("webhook HTTP retry (%d left): %s url=%s", r->retry_left, why, r->url);
        r->timer = zn_poller_do_delay(r->io, (uint64_t)delay, req_retry_cb, r);
        if (r->timer) {
            return;
        }
    }
    req_finish(r, -1);
}

static uint64_t req_timeout_cb(void *user)
{
    zms_hook_req *r = (zms_hook_req *)user;

    r->timer = NULL;
    req_fail(r, "timeout");
    return 0;
}

/** 响应是否收全：以 Content-Length 为准；无长度则等对端关闭。 */
static int resp_ready(const zms_hook_req *r)
{
    const char *hdr_end = strstr(r->resp, "\r\n\r\n");
    const char *cl;
    long want;
    size_t body;

    if (!hdr_end) {
        return 0;
    }
    cl = strstr(r->resp, "Content-Length:");
    if (!cl || cl > hdr_end) {
        cl = strstr(r->resp, "content-length:");
        if (!cl || cl > hdr_end) {
            return 0;
        }
    }
    want = strtol(cl + 15, NULL, 10);
    body = r->resp_len - (size_t)(hdr_end + 4 - r->resp);
    return want >= 0 && body >= (size_t)want;
}

static void req_on_connect(zn_tcp_client *cli, void *user)
{
    zms_hook_req *r = (zms_hook_req *)user;

    r->awaiting_response = 1;
    if (zn_tcp_client_send(cli, r->req, r->req_len) != ZN_OK) {
        req_fail(r, "send failed");
    }
}

static void req_on_recv(zn_tcp_client *cli, const void *data, size_t len, void *user)
{
    zms_hook_req *r = (zms_hook_req *)user;
    size_t room = sizeof(r->resp) - 1 - r->resp_len;

    (void)cli;
    if (len > room) {
        len = room;
    }
    if (len > 0) {
        memcpy(r->resp + r->resp_len, data, len);
        r->resp_len += len;
        r->resp[r->resp_len] = '\0';
    }
    if (resp_ready(r)) {
        req_finish(r, 0);
    }
}

static void req_on_error(zn_tcp_client *cli, void *user)
{
    zms_hook_req *r = (zms_hook_req *)user;

    (void)cli;
    /* 无 Content-Length 的响应以对端关闭结束，此时视为成功。 */
    if (r->awaiting_response && strstr(r->resp, "\r\n\r\n")) {
        req_finish(r, 0);
        return;
    }
    req_fail(r, "connection error");
}

static const zn_tcp_client_ops_t g_req_ops = { req_on_connect, req_on_recv, req_on_error };

static void req_start(zms_hook_req *r)
{
    int timeout_ms = (int)(g_hook.timeout_sec * 1000.f);

    if (timeout_ms <= 0) {
        timeout_ms = 10000;
    }
    if (!r->cli) {
        zn_tcp_client_opts_t opts;

        opts.poller = r->io;
        opts.ops = &g_req_ops;
        opts.user = r;
        r->cli = zn_tcp_client_create(&opts);
        if (!r->cli) {
            req_finish(r, -1);
            return;
        }
    }
    r->awaiting_response = 0;
    r->resp_len = 0;
    r->resp[0] = '\0';

    /* 先挂超时：connect 可能同步完成并触发 on_connect。 */
    r->timer = zn_poller_do_delay(r->io, (uint64_t)timeout_ms, req_timeout_cb, r);
    if (!r->timer) {
        req_finish(r, -1);
        return;
    }
    if (zn_tcp_client_connect(r->cli, r->host, r->port) != ZN_OK) {
        req_fail(r, "connect failed");
    }
}

static void req_start_with_body(zms_hook_req *r, const char *body)
{
    size_t body_len = body ? strlen(body) : 0;
    size_t cap = body_len + 512;
    int n;

    r->req = (char *)malloc(cap);
    if (!r->req) {
        req_free(r);
        return;
    }
    n = snprintf(r->req, cap,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 r->path, r->host, (unsigned)r->port, body_len, body ? body : "");
    if (n < 0 || (size_t)n >= cap) {
        req_free(r);
        return;
    }
    r->req_len = (size_t)n;
    req_start(r);
}

/** 主 poller 线程：把一条通知变成非阻塞请求。 */
static void hook_notify_post(void *user)
{
    zms_hook_notify *n = (zms_hook_notify *)user;
    zms_hook_req *r;
    char ip[64];

    if (!n) {
        return;
    }
    if (!g_webhook_initialized || !g_poller) {
        free(n->body);
        free(n);
        return;
    }
    r = (zms_hook_req *)calloc(1, sizeof(*r));
    if (!r) {
        free(n->body);
        free(n);
        return;
    }
    r->io = g_poller;
    r->retry_left = g_hook.retry_count > 0 ? g_hook.retry_count : 0;
    r->none_reader = n->none_reader;
    if (n->has_tuple) {
        r->tuple = n->tuple;
        r->has_tuple = 1;
    }
    snprintf(r->url, sizeof(r->url), "%s", n->url);
    if (parse_http_url(n->url, r->host, sizeof(r->host), &r->port, r->path, sizeof(r->path)) != 0) {
        zn_warn("webhook: bad url %s", n->url);
        free(n->body);
        free(n);
        free(r);
        return;
    }
    if (host_ip_lookup(r->host, ip, sizeof(ip))) {
        snprintf(r->host, sizeof(r->host), "%s", ip);
    } else {
        /* poller 上不做 DNS：未在 init 解析过的主机直接放弃。 */
        zn_warn("webhook: host %s not resolved at init, drop notification", r->host);
        free(n->body);
        free(n);
        free(r);
        return;
    }
    r->next = g_reqs;
    g_reqs = r;
    ++g_hook_pending;
    req_start_with_body(r, n->body);
    free(n->body);
    free(n);
}

/** 任意线程：打包通知并投递到主 poller（body 所有权转移）。 */
static void hook_queue(const char *url, char *body, const zms_media_tuple *tuple, int none_reader)
{
    zms_hook_notify *n;

    if (!body) {
        return;
    }
    if (!g_webhook_initialized || !g_poller || !url || !url[0]) {
        free(body);
        return;
    }
    n = (zms_hook_notify *)calloc(1, sizeof(*n));
    if (!n) {
        free(body);
        return;
    }
    snprintf(n->url, sizeof(n->url), "%s", url);
    n->body = body;
    n->none_reader = none_reader;
    if (tuple) {
        n->tuple = *tuple;
        n->has_tuple = 1;
    }
    if (zn_poller_async(g_poller, hook_notify_post, n, 1) != ZN_OK) {
        free(n->body);
        free(n);
    }
}

/* ── 通知入口 ─────────────────────────────────────────────────────────────── */

static char *body_alloc(void)
{
    return (char *)malloc(ZMS_HOOK_BODY_MAX);
}

/*
 * 组装自注册/心跳 JSON 体：包含本节点对外地址、各协议实际监听端口、协议能力
 * 开关与 API secret。camelCase 键与 zero-web-server 的 RegisterRequest 对齐。
 * 返回 0 成功。
 */
static int build_server_info(char *body, size_t cap, const zms_config *cfg,
                             const zms_media_server_ports *ports)
{
    const char *ip;
    const char *secret;
    size_t len;
    int n;
    int enable_srt;
    int enable_rtsp;
    int enable_rtmp;
    int enable_hls;
    int enable_webrtc;

    if (!body || !cfg || !ports) {
        return -1;
    }
    ip = cfg->general.extern_ip[0] ? cfg->general.extern_ip : "127.0.0.1";
    secret = cfg->http.api_secret ? cfg->http.api_secret : "";
    enable_rtsp = cfg->protocol.enable_rtsp ? 1 : 0;
    enable_rtmp = cfg->protocol.enable_rtmp ? 1 : 0;
    enable_hls = cfg->protocol.enable_hls ? 1 : 0;
    enable_webrtc = cfg->webrtc.enable ? 1 : 0;
    enable_srt = (cfg->protocol.enable_srt && ports->srt) ? 1 : 0;

    n = snprintf(
        body, cap,
        "{\"version\":\"%s\",\"ip\":\"%s\",\"hookIp\":\"%s\",\"sdpIp\":\"%s\","
        "\"streamIp\":\"%s\",\"secret\":\"%s\","
        "\"httpPort\":%u,\"httpSslPort\":%u,"
        "\"rtspPort\":%u,\"rtspSslPort\":%u,"
        "\"rtmpPort\":%u,\"rtmpSslPort\":%u,"
        "\"srtPort\":%u,\"webrtcPortMin\":%u,\"webrtcPortMax\":%u,"
        "\"enableRtsp\":%s,\"enableRtmp\":%s,\"enableHls\":%s,"
        "\"enableSrt\":%s,\"enableWebrtc\":%s",
        ZMS_VERSION_STRING, ip, ip, ip, ip, secret, ports->http, cfg->http.ssl_port,
        ports->rtsp, cfg->rtsp.ssl_port, ports->rtmp, cfg->rtmp.ssl_port, ports->srt,
        cfg->webrtc.port_min, cfg->webrtc.port_max, enable_rtsp ? "true" : "false",
        enable_rtmp ? "true" : "false", enable_hls ? "true" : "false",
        enable_srt ? "true" : "false", enable_webrtc ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    len = (size_t)n;
    if (json_append_common(body, cap, &len, ",") != 0) {
        return -1;
    }
    if ((size_t)snprintf(body + len, cap - len, "}") >= cap - len) {
        return -1;
    }
    return 0;
}

void zms_webhook_server_started(const zms_config *cfg, const zms_media_server_ports *ports)
{
    char *body;

    if (!g_webhook_initialized || !g_hook.on_server_started[0] || !cfg || !ports) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    if (build_server_info(body, ZMS_HOOK_BODY_MAX, cfg, ports) != 0) {
        free(body);
        return;
    }
    zn_info("[hook] on_server_started -> %s (http=%u rtsp=%u rtmp=%u srt=%u)",
            g_hook.on_server_started, ports->http, ports->rtsp, ports->rtmp, ports->srt);
    hook_queue(g_hook.on_server_started, body, NULL, 0);
}

/* 心跳定时回调：发一次注册体并返回下次间隔（毫秒）；关闭后返回 0 自动终止。 */
static uint64_t keepalive_tick(void *user)
{
    char *body;
    int interval;

    (void)user;
    if (!g_kp_enabled || !g_webhook_initialized || !g_hook.on_server_keepalive[0] || !g_kp_cfg) {
        return 0;
    }
    body = body_alloc();
    if (body) {
        if (build_server_info(body, ZMS_HOOK_BODY_MAX, g_kp_cfg, &g_kp_ports) == 0) {
            hook_queue(g_hook.on_server_keepalive, body, NULL, 0);
        } else {
            free(body);
        }
    }
    interval = g_hook.keepalive_interval_sec > 0 ? g_hook.keepalive_interval_sec : 30;
    return (uint64_t)interval * 1000u;
}

static void keepalive_arm(void *user)
{
    int interval;

    (void)user;
    if (!g_kp_enabled || !g_poller) {
        return;
    }
    interval = g_hook.keepalive_interval_sec > 0 ? g_hook.keepalive_interval_sec : 30;
    g_kp_timer = zn_poller_do_delay(g_poller, (uint64_t)interval * 1000u, keepalive_tick, NULL);
}

void zms_webhook_start_keepalive(const zms_config *cfg, const zms_media_server_ports *ports)
{
    if (!g_webhook_initialized || !cfg || !ports || !g_hook.on_server_keepalive[0]) {
        return;
    }
    g_kp_cfg = cfg;
    g_kp_ports = *ports;
    g_kp_enabled = 1;
    if (g_poller) {
        (void)zn_poller_async(g_poller, keepalive_arm, NULL, 1);
    }
    zn_info("[hook] keepalive started -> %s interval=%ds", g_hook.on_server_keepalive,
            g_hook.keepalive_interval_sec > 0 ? g_hook.keepalive_interval_sec : 30);
}

void zms_webhook_stop_keepalive(void)
{
    g_kp_enabled = 0;
    g_kp_cfg = NULL;
    if (g_kp_timer) {
        zn_poller_timer_cancel(g_kp_timer);
        g_kp_timer = NULL;
    }
}

void zms_webhook_on_stream_register(zms_media_source *src, zms_media_origin origin)
{
    char *body;

    (void)origin;
    if (!g_webhook_initialized || !g_hook.on_stream_changed[0] || !src) {
        return;
    }
    if (!schema_allowed(src->schema)) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    if (build_stream_changed_regist(src, 1, body, ZMS_HOOK_BODY_MAX) != 0) {
        free(body);
        return;
    }
    zn_debug("[hook] on_stream_changed regist=1 app=%s stream=%s -> %s", src->app, src->stream,
              g_hook.on_stream_changed);
    hook_queue(g_hook.on_stream_changed, body, NULL, 0);
}

void zms_webhook_on_stream_unregister(zms_media_source *src, zms_media_origin origin)
{
    char *body;

    (void)origin;
    if (!g_webhook_initialized || !g_hook.on_stream_changed[0] || !src) {
        return;
    }
    if (!schema_allowed(src->schema)) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    if (build_stream_changed_regist(src, 0, body, ZMS_HOOK_BODY_MAX) != 0) {
        free(body);
        return;
    }
    hook_queue(g_hook.on_stream_changed, body, NULL, 0);
}

void zms_webhook_on_play_stop(const zms_media_tuple *tuple, const char *player_schema,
                              uint64_t duration_ms)
{
    char *body;
    size_t len;

    if (!g_webhook_initialized || !g_hook.on_play_stop[0] || !tuple) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    if (build_tuple_body(tuple, player_schema, body, ZMS_HOOK_BODY_MAX) != 0) {
        free(body);
        return;
    }
    /* 已知时在闭合花括号前追加 durationMs。 */
    if (duration_ms > 0) {
        char extra[48];
        int n;

        len = strlen(body);
        if (len > 0 && body[len - 1] == '}') {
            n = snprintf(extra, sizeof(extra), ",\"durationMs\":%llu}",
                         (unsigned long long)duration_ms);
            if (n > 0 && len - 1 + (size_t)n < ZMS_HOOK_BODY_MAX) {
                body[len - 1] = '\0';
                strncat(body, extra, ZMS_HOOK_BODY_MAX - len);
            }
        }
    }
    hook_queue(g_hook.on_play_stop, body, NULL, 0);
}

void zms_webhook_on_none_reader(const zms_media_tuple *tuple)
{
    char *body;

    if (!g_webhook_initialized || !g_hook.on_stream_none_reader[0] || !tuple) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    if (build_tuple_body(tuple, NULL, body, ZMS_HOOK_BODY_MAX) != 0) {
        free(body);
        return;
    }
    hook_queue(g_hook.on_stream_none_reader, body, tuple, 1);
}

static void json_escape_path(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;

    if (!out || out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (; *in && o + 2 < out_cap; ++in) {
        char c = *in;

        if (c == '\\') {
            out[o++] = '/';
        } else if (c == '"') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = '"';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static int record_file_to_http_rel(const char *file_path, char *out, size_t out_cap)
{
    char norm[ZMS_CFG_PATH_MAX];
    const char *p;
    size_t i;

    if (!file_path || !file_path[0] || !out || out_cap == 0) {
        return 0;
    }
    strncpy(norm, file_path, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';
    for (i = 0; norm[i]; ++i) {
        if (norm[i] == '\\') {
            norm[i] = '/';
        }
    }
    p = strstr(norm, "/record/");
    if (p) {
        p += 1; /* "record/..." */
    } else if (strncmp(norm, "record/", 7) == 0) {
        p = norm;
    } else if (strncmp(norm, "./record/", 9) == 0) {
        p = norm + 2;
    } else {
        return 0;
    }
    if (strstr(p, "..") != NULL) {
        return 0;
    }
    return (size_t)snprintf(out, out_cap, "%s", p) < out_cap;
}

void zms_webhook_on_record_mp4(const char *app, const char *stream, const char *file_name,
                               const char *file_path, const char *folder, int64_t file_size,
                               int64_t start_time, float time_len)
{
    char *body;
    char esc_name[256];
    char esc_path[ZMS_CFG_PATH_MAX];
    char esc_folder[ZMS_CFG_PATH_MAX];
    char esc_url[ZMS_CFG_URL_MAX];
    char rel[ZMS_CFG_PATH_MAX];
    char url[ZMS_CFG_URL_MAX];
    size_t len = 0;
    int n;

    if (!g_webhook_initialized || !g_hook.on_record_mp4[0]) {
        return;
    }
    body = body_alloc();
    if (!body) {
        return;
    }
    json_escape_path(file_name ? file_name : "", esc_name, sizeof(esc_name));
    json_escape_path(file_path ? file_path : "", esc_path, sizeof(esc_path));
    json_escape_path(folder ? folder : "", esc_folder, sizeof(esc_folder));
    url[0] = '\0';
    esc_url[0] = '\0';
    if (record_file_to_http_rel(file_path, rel, sizeof(rel)) && g_play_host[0] && g_play_http_port) {
        n = snprintf(url, sizeof(url), "http://%s:%u/%s", g_play_host, g_play_http_port, rel);
        if (n > 0 && (size_t)n < sizeof(url)) {
            json_escape_path(url, esc_url, sizeof(esc_url));
        }
    }
    n = snprintf(body, ZMS_HOOK_BODY_MAX,
                 "{\"app\":\"%s\",\"stream\":\"%s\",\"file_name\":\"%s\",\"file_path\":\"%s\","
                 "\"folder\":\"%s\",\"file_size\":%lld,\"start_time\":%lld,\"time_len\":%.3f",
                 app ? app : "", stream ? stream : "", esc_name, esc_path, esc_folder,
                 (long long)file_size, (long long)start_time, (double)time_len);
    if (n < 0 || (size_t)n >= ZMS_HOOK_BODY_MAX) {
        free(body);
        return;
    }
    len = (size_t)n;
    if (esc_url[0]) {
        n = snprintf(body + len, ZMS_HOOK_BODY_MAX - len, ",\"url\":\"%s\",\"play_url\":\"%s\"",
                     esc_url, esc_url);
        if (n < 0 || (size_t)n >= ZMS_HOOK_BODY_MAX - len) {
            free(body);
            return;
        }
        len += (size_t)n;
    }
    if (json_append_common(body, ZMS_HOOK_BODY_MAX, &len, ",") != 0 ||
        (size_t)snprintf(body + len, ZMS_HOOK_BODY_MAX - len, "}") >= ZMS_HOOK_BODY_MAX - len) {
        free(body);
        return;
    }
    hook_queue(g_hook.on_record_mp4, body, NULL, 0);
}

/* ── 生命周期 ────────────────────────────────────────────────────────────── */

/** 在 init 线程把每个已配置钩子 URL 的主机名解析成 IP（poller 上不再 DNS）。 */
static void resolve_hook_hosts(void)
{
    const char *urls[6];
    int n = 0;

    urls[n++] = g_hook.on_stream_changed;
    urls[n++] = g_hook.on_stream_none_reader;
    urls[n++] = g_hook.on_play_stop;
    urls[n++] = g_hook.on_server_started;
    urls[n++] = g_hook.on_server_keepalive;
    urls[n++] = g_hook.on_record_mp4;

    g_host_count = 0;
    for (int i = 0; i < n; ++i) {
        char host[256];
        char path[512];
        uint16_t port = 80;
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        char port_str[8];
        int dup = 0;

        if (!urls[i] || !urls[i][0] || parse_http_url(urls[i], host, sizeof(host), &port, path,
                                                     sizeof(path)) != 0) {
            continue;
        }
        for (int k = 0; k < g_host_count; ++k) {
            if (strcmp(g_hosts[k].host, host) == 0) {
                dup = 1;
            }
        }
        if (dup || g_host_count >= ZMS_HOOK_HOST_MAX) {
            continue;
        }
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(g_hosts[g_host_count].host, sizeof(g_hosts[g_host_count].host), "%s", host);
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
            zn_warn("webhook: resolve %s failed, notifications to it will be dropped", host);
            ++g_host_count;
            continue;
        }
        if (res->ai_family == AF_INET) {
            (void)inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
                            g_hosts[g_host_count].ip, sizeof(g_hosts[g_host_count].ip));
        } else if (res->ai_family == AF_INET6) {
            (void)inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr,
                            g_hosts[g_host_count].ip, sizeof(g_hosts[g_host_count].ip));
        }
        freeaddrinfo(res);
        ++g_host_count;
    }
}

void zms_webhook_init(zn_poller *poller, const zms_config *cfg)
{
    g_poller = poller;
    g_webhook_initialized = 0;
    g_play_host[0] = '\0';
    g_play_http_port = 0;
    g_host_count = 0;
    if (!cfg) {
        return;
    }
    g_hook = cfg->hook;
    g_webhook_initialized = g_hook.enable && poller != NULL;
    if (g_webhook_initialized) {
        resolve_hook_hosts();
    }
    if (cfg->general.extern_ip[0]) {
        strncpy(g_play_host, cfg->general.extern_ip, sizeof(g_play_host) - 1);
    } else {
        strncpy(g_play_host, "127.0.0.1", sizeof(g_play_host) - 1);
    }
    g_play_host[sizeof(g_play_host) - 1] = '\0';
    g_play_http_port = cfg->http.port ? cfg->http.port : 80;
    zn_info("webhook init enable=%d on_stream_changed=%s mediaServerId=%s play_base=http://%s:%u",
             g_webhook_initialized,
             g_hook.on_stream_changed[0] ? g_hook.on_stream_changed : "(none)",
             g_hook.media_server_id[0] ? g_hook.media_server_id : "(none)", g_play_host,
             g_play_http_port);
}

void zms_webhook_fini(void)
{
    g_webhook_initialized = 0;
    zms_webhook_stop_keepalive();
    if (!g_poller) {
        return;
    }
    /* 排空已投递的通知，并给在途 HTTP 留出收尾时间（主 poller 仍在运行）。 */
    {
        int wait_ms = (int)(g_hook.timeout_sec * 1000.f) + 1000;
        int waited = 0;

        if (wait_ms < 1000) {
            wait_ms = 1000;
        }
        while (waited < wait_ms && g_hook_pending > 0) {
            zn_poller_wake(g_poller);
            zn_poller_process_pending(g_poller);
            zn_sleep_ms(10);
            waited += 10;
        }
        if (g_hook_pending > 0) {
            zn_warn("webhook fini: %d notification(s) still in flight", (int)g_hook_pending);
        }
    }
    g_poller = NULL;
}

int zms_webhook_none_reader_configured(void)
{
    return g_webhook_initialized && g_hook.on_stream_none_reader[0] != '\0';
}
