/**
 * @file ts_route.c
 * @brief HTTP-TS 路由注册。
 */
#include "zcs/protocol/http/route.h"
#include "session/http/session_internal.h"
#include "zcs/core/stream/hub.h"
#include <string.h>

static zcs_media_source *http_find_play_src(const zcs_http_request *req, const char *app,
                                           const char *stream)
{
    char vhost[ZCS_VHOST_MAX];

    zcs_http_request_vhost(req, vhost, sizeof(vhost));
    return zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, vhost, app, stream);
}



static int http_ts_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];

    if (!req) {
        return 0;
    }
    if (!zcs_http_route_parse_live_ts_path(req->path, app, stream)) {
        return 0;
    }
    return app[0] && stream[0];
}

static void http_ts_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zcs_http_route_parse_live_ts_path(req->path, app, stream)) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || zcs_media_source_is_vod(src) || !zcs_media_source_use_gop_queue_play(src)) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_WS_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    zcs_http_live_ts_start(hs, src, app, stream);
}

static const zcs_http_route_ops k_http_ts_route = {
    .name = "http-ts-live",
    .match = http_ts_route_match,
    .handle = http_ts_route_handle,
};

void zcs_http_ts_routes_register(void)
{
    zcs_http_route_register(&k_http_ts_route);
}
