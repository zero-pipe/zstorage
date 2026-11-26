/**
 * @file webapi_proxy.c
 * @brief Web API 拉流代理：add / del / list。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zms/pull/proxy.h"
#include "zms/ops/config/pull_ssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* 与 webapi_rtp.c 共用：把 rtp_transport 查询参数映射为 RTP 模式。 */
zms_rtsp_rtp_mode webapi_parse_rtp_transport_param(const char *s);

static int proxy_stream_is_auto(const char *stream)
{
    if (!stream || !stream[0]) {
        return 1;
    }
    return strcmp(stream, "auto") == 0;
}
typedef struct zms_webapi_proxy_list_ctx {
    zms_webapi_json_buf *jb;
    int first;
} zms_webapi_proxy_list_ctx;

static int proxy_list_visit(const char *key, zms_live_pull_proxy *p, void *user)
{
    zms_webapi_proxy_list_ctx *ctx = (zms_webapi_proxy_list_ctx *)user;
    (void)p;
    if (webapi_jb_append(ctx->jb, "%s\"%s\"", ctx->first ? "" : ",", key) != 0) {
        return -1;
    }
    ctx->first = 0;
    return 0;
}

size_t webapi_handle_proxy_list(zms_webapi_json_buf *jb)
{
    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zms_webapi_proxy_list_ctx ctx = {jb, 1};
    zms_live_pull_proxy_foreach(proxy_list_visit, &ctx);
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_proxy_add(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                      char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    if (!opts || !opts->poller) {
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "no poller for addStreamProxy", NULL, body,
                            body_cap);
    }
    if (!q->url[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing url", NULL, body, body_cap);
    }

    const zms_config *cfg = opts->cfg;
    const char *app = q->app[0] ? q->app : (cfg && cfg->proxy.app[0] ? cfg->proxy.app : "live");
    const char *prefix = q->proxy_prefix[0]
                             ? q->proxy_prefix
                             : (cfg && cfg->proxy.prefix[0] ? cfg->proxy.prefix : NULL);

    char stream_auto[ZMS_STREAM_MAX];
    const char *stream = q->stream;
    if (proxy_stream_is_auto(q->stream)) {
        if (zms_live_pull_proxy_build_stream(q->url, prefix, stream_auto, sizeof(stream_auto)) !=
            0) {
            return webapi_respond_json(ZMS_API_INVALID_ARGS, "derive stream name failed", NULL, body,
                                body_cap);
        }
        stream = stream_auto;
    }

    char key[256];
    zms_live_pull_proxy_make_key(q->vhost[0] ? q->vhost : NULL, app, stream, key, sizeof(key));
    zms_live_pull_proxy *old = zms_live_pull_proxy_find_by_key(key);
    if (old) {
        zms_live_pull_proxy_destroy(old);
    }

    zms_live_pull_proxy_opts popts = {
        .poller = opts->poller,
        .pull_url = q->url,
        .ssl_ctx = zms_pull_ssl_ctx(cfg),
        .vhost = q->vhost[0] ? q->vhost : NULL,
        .app = app,
        .stream = stream,
        .proxy_prefix = prefix,
        .rtp_mode = webapi_parse_rtp_transport_param(q->rtp_transport),
        .retry_count = -1,
        .reconnect_delay_ms = 3000,
    };
    zms_live_pull_proxy *proxy = zms_live_pull_proxy_create(&popts);
    if (!proxy || zms_live_pull_proxy_start(proxy) != ZN_OK) {
        zms_live_pull_proxy_destroy(proxy);
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "addStreamProxy failed", NULL, body, body_cap);
    }
    return (size_t)snprintf(
        body, body_cap, "{\"code\":0,\"data\":{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\"}}",
        key, zms_live_pull_proxy_app(proxy), zms_live_pull_proxy_stream(proxy));
}

size_t webapi_handle_proxy_del(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    *http_status = 200;
    if (!q->key[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing key", NULL, body, body_cap);
    }
    zms_live_pull_proxy *p = zms_live_pull_proxy_find_by_key(q->key);
    int flag = 0;
    if (p) {
        zms_live_pull_proxy_destroy(p);
        flag = 1;
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"flag\":%s}}",
                            flag ? "true" : "false");
}