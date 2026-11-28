/**
 * @file webapi_rtp.c
 * @brief Web API RTP-PS 服务：open / connect / close / list。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zcs/protocol/rtp/ps_server.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

zcs_rtsp_rtp_mode webapi_parse_rtp_transport_param(const char *s)
{
    if (!s || !s[0]) {
        return ZCS_RTSP_RTP_TCP;
    }
#ifdef _WIN32
    if (_stricmp(s, "udp") == 0) {
        return ZCS_RTSP_RTP_UDP;
    }
    if (_stricmp(s, "auto") == 0) {
        return ZCS_RTSP_RTP_AUTO;
    }
#else
    if (strcasecmp(s, "udp") == 0) {
        return ZCS_RTSP_RTP_UDP;
    }
    if (strcasecmp(s, "auto") == 0) {
        return ZCS_RTSP_RTP_AUTO;
    }
#endif
    return ZCS_RTSP_RTP_TCP;
}
typedef struct zcs_webapi_rtp_ps_list_ctx {
    zcs_webapi_json_buf *jb;
    int first;
} zcs_webapi_rtp_ps_list_ctx;

static int rtp_ps_list_visit(const char *key, zcs_rtp_ps_server_slot *slot, void *user)
{
    zcs_webapi_rtp_ps_list_ctx *ctx = (zcs_webapi_rtp_ps_list_ctx *)user;

    if (!ctx || !ctx->jb || !slot) {
        return 0;
    }
    if (!ctx->first && webapi_jb_append(ctx->jb, ",") != 0) {
        return -1;
    }
    ctx->first = 0;
    if (webapi_jb_append(
            ctx->jb, "{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\",\"port\":%u,\"pt\":%d}", key,
            zcs_rtp_ps_server_app(slot), zcs_rtp_ps_server_stream(slot),
            (unsigned)zcs_rtp_ps_server_port(slot), zcs_rtp_ps_server_payload_type(slot)) != 0) {
        return -1;
    }
    return 0;
}

size_t webapi_handle_rtp_list(zcs_webapi_json_buf *jb)
{
    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    zcs_webapi_rtp_ps_list_ctx ctx = {jb, 1};
    zcs_rtp_ps_server_foreach(rtp_ps_list_visit, &ctx);
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_rtp_open(const zcs_webapi_query_map *q, const zcs_web_api_opts *opts,
                                     char *body, size_t body_cap, int *http_status)
{
    zcs_rtp_ps_server_open_opts ropts;
    zcs_rtp_ps_server_slot *slot = NULL;
    uint16_t bound_port = 0;
    const char *app;
    const char *stream;
    char key[256];

    *http_status = 200;
    if (!opts || !opts->poller) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "no poller for openRtpServer", NULL, body,
                            body_cap);
    }

    app = q->app[0] ? q->app : "live";
    stream = q->stream[0] ? q->stream : (q->stream_id[0] ? q->stream_id : NULL);
    if (!stream) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing stream", NULL, body, body_cap);
    }

    memset(&ropts, 0, sizeof(ropts));
    ropts.poller = opts->poller;
    ropts.vhost = q->vhost[0] ? q->vhost : NULL;
    ropts.app = app;
    ropts.stream = stream;
    ropts.port = q->port[0] ? (uint16_t)atoi(q->port) : 0;
    ropts.payload_type = q->pt[0] ? atoi(q->pt) : ZCS_RTP_PS_DEFAULT_PT;
    if (q->ssrc[0]) {
        ropts.ssrc = (uint32_t)strtoul(q->ssrc, NULL, 10);
        ropts.enable_ssrc_filter = 1;
    }
    ropts.tcp_mode = q->tcp_mode[0] ? atoi(q->tcp_mode) : ZCS_RTP_PS_TCP_UDP;
    ropts.poller_pool = opts->poller_pool;
    ropts.params = q->raw_query[0] ? q->raw_query : NULL;

    {
        zn_err_t r = zcs_rtp_ps_server_open(&ropts, &slot, &bound_port);

        if (r != ZN_OK || !slot) {
            if (r == ZN_ERR_DENIED) {
                *http_status = 403;
                return webapi_respond_json(ZCS_API_AUTH_FAILED, "openRtpServer denied", NULL, body,
                                    body_cap);
            }
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "openRtpServer failed", NULL, body,
                                body_cap);
        }
    }

    zcs_rtp_ps_server_make_key(ropts.vhost, app, stream, key, sizeof(key));
    zn_info(
        "[GB28181 zcs 2/6] openRtpServer app=%s stream=%s port=%u pt=%d tcp_mode=%d ssrc_filter=%s",
        app, stream, (unsigned)bound_port, zcs_rtp_ps_server_payload_type(slot), ropts.tcp_mode,
        q->ssrc[0] ? q->ssrc : "off");
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":{\"key\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\","
                            "\"port\":%u,\"pt\":%d}}",
                            key, app, stream, (unsigned)bound_port,
                            zcs_rtp_ps_server_payload_type(slot));
}

size_t webapi_handle_rtp_connect(const zcs_webapi_query_map *q, const zcs_web_api_opts *opts,
                                        char *body, size_t body_cap, int *http_status)
{
    const char *app;
    const char *stream;
    const char *host;
    uint16_t port;

    *http_status = 200;
    if (!opts || !opts->poller) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "no poller for connectRtpServer", NULL, body,
                            body_cap);
    }

    app = q->app[0] ? q->app : "live";
    stream = q->stream[0] ? q->stream : (q->stream_id[0] ? q->stream_id : NULL);
    host = q->dst_url[0] ? q->dst_url : NULL;
    port = q->dst_port[0] ? (uint16_t)atoi(q->dst_port) : 0;
    if (!stream || !host || port == 0) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing stream/dst_url/dst_port", NULL, body,
                            body_cap);
    }

    if (zcs_rtp_ps_server_connect(opts->poller, q->vhost[0] ? q->vhost : NULL, app, stream, host,
                                  port) != ZN_OK) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "connectRtpServer failed", NULL, body, body_cap);
    }

    zn_debug("[GB28181 zcs 3/6] connectRtpServer app=%s stream=%s -> %s:%u", app, stream, host,
              (unsigned)port);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\"}");
}

size_t webapi_handle_rtp_close(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    zcs_rtp_ps_server_slot *slot = NULL;
    int flag = 0;

    *http_status = 200;
    if (q->key[0]) {
        slot = zcs_rtp_ps_server_find_by_key(q->key);
    } else if (q->app[0] && (q->stream[0] || q->stream_id[0])) {
        char key[256];
        const char *stream = q->stream[0] ? q->stream : q->stream_id;
        zcs_rtp_ps_server_make_key(q->vhost[0] ? q->vhost : NULL, q->app, stream, key, sizeof(key));
        slot = zcs_rtp_ps_server_find_by_key(key);
    }
    if (slot) {
        zcs_rtp_ps_server_close(slot);
        flag = 1;
    }
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"data\":{\"flag\":%s}}",
                            flag ? "true" : "false");
}