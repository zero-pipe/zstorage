/**
 * @file webapi_proxy.c
 * @brief Web API 拉流代理：add / del / list。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zcs/pull/proxy.h"
#include "zcs/ops/config/pull_ssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* 与 webapi_rtp.c 共用：把 rtp_transport 查询参数映射为 RTP 模式。 */
zcs_rtsp_rtp_mode webapi_parse_rtp_transport_param(const char *s);

static int proxy_stream_is_auto(const char *stream)
{
    if (!stream || !stream[0]) {
        return 1;
    }
    return strcmp(stream, "auto") == 0;
}
typedef struct zcs_webapi_proxy_list_ctx {
    zcs_webapi_json_buf *jb;
    int first;
} zcs_webapi_proxy_list_ctx;

static int proxy_list_visit(const char *key, zcs_live_pull_proxy *p, void *user)
{
    zcs_webapi_proxy_list_ctx *ctx = (zcs_webapi_proxy_list_ctx *)user;
    (void)p;
    if (webapi_jb_append(ctx->jb, "%s\"%s\"", ctx->first ? "" : ",", key) != 0) {
        return -1;
    }
    ctx->first = 0;
    return 0;
}

size_t webapi_handle_proxy_list(zcs_webapi_json_buf *jb)
{
    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zcs_webapi_proxy_list_ctx ctx = {jb, 1};
    zcs_live_pull_proxy_foreach(proxy_list_visit, &ctx);
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_proxy_add(const zcs_webapi_query_map *q, const zcs_web_api_opts *opts,
                                      char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    if (!opts || !opts->poller) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "no poller for addStreamProxy", NULL, body,
                            body_cap);
    }
    if (!q->url[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing url", NULL, body, body_cap);
    }

    const zcs_config *cfg = opts->cfg;
    const char *app = q->app[0] ? q->app : (cfg && cfg->proxy.app[0] ? cfg->proxy.app : "live");
    const char *prefix = q->proxy_prefix[0]
                             ? q->proxy_prefix
                             : (cfg && cfg->proxy.prefix[0] ? cfg->proxy.prefix : NULL);

    char stream_auto[ZCS_STREAM_MAX];
    const char *stream = q->stream;
    if (proxy_stream_is_auto(q->stream)) {
        if (zcs_live_pull_proxy_build_stream(q->url, prefix, stream_auto, sizeof(stream_auto)) !=
            0) {
            return webapi_respond_json(ZCS_API_INVALID_ARGS, "derive stream name failed", NULL, body,
                                body_cap);
        }
        stream = stream_auto;
    }

    char key[256];
    zcs_live_pull_proxy_make_key(q->vhost[0] ? q->vhost : NULL, app, stream, key, sizeof(key));
    zcs_live_pull_proxy *old = zcs_live_pull_proxy_find_by_key(key);
    if (old) {
        zcs_live_pull_proxy_destroy(old);
    }

    zcs_live_pull_proxy_opts popts = {
        .poller = opts->poller,
        .pull_url = q->url,
        .ssl_ctx = zcs_pull_ssl_ctx(cfg),
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = app,
        .stream = stream,
        .proxy_prefix = prefix,
        .rtp_mode = webapi_parse_rtp_transport_param(q->rtp_transport),
        .retry_count = -1,
        .reconnect_delay_ms = 3000,
    };
    zcs_live_pull_proxy *proxy = zcs_live_pull_proxy_create(&popts);
    if (!proxy || zcs_live_pull_proxy_start(proxy) != ZN_OK) {
        zcs_live_pull_proxy_destroy(proxy);
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "addStreamProxy failed", NULL, body, body_cap);
    }
    return (size_t)snprintf(
        body, body_cap, "{\"code\":0,\"data\":{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\"}}",
        key, zcs_live_pull_proxy_app(proxy), zcs_live_pull_proxy_stream(proxy));
}

size_t webapi_handle_proxy_del(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    *http_status = 200;
    if (!q->key[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing key", NULL, body, body_cap);
    }
    zcs_live_pull_proxy *p = zcs_live_pull_proxy_find_by_key(q->key);
    int flag = 0;
    if (p) {
        zcs_live_pull_proxy_destroy(p);
        flag = 1;
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"flag\":%s}}",
                            flag ? "true" : "false");
}