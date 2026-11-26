/**
 * @file ts_route.c
 * @brief HTTP-TS 路由注册。
 */
#include "zms/protocol/http/route.h"
#include "session/http/session_internal.h"
#include "zms/core/stream/hub.h"
#include <string.h>

static zms_media_source *http_find_play_src(const zms_http_request *req, const char *app,
                                           const char *stream)
{
    char vhost[ZMS_VHOST_MAX];

    zms_http_request_vhost(req, vhost, sizeof(vhost));
    return zms_media_source_find_for_play_ex(ZMS_SCHEMA_RTMP, vhost, app, stream);
}



static int http_ts_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (!req) {
        return 0;
    }
    if (!zms_http_route_parse_live_ts_path(req->path, app, stream)) {
        return 0;
    }
    return app[0] && stream[0];
}

static void http_ts_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zms_http_route_parse_live_ts_path(req->path, app, stream)) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || zms_media_source_is_vod(src) || !zms_media_source_use_gop_queue_play(src)) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_WS_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    zms_http_live_ts_start(hs, src, app, stream);
}

static const zms_http_route_ops k_http_ts_route = {
    .name = "http-ts-live",
    .match = http_ts_route_match,
    .handle = http_ts_route_handle,
};

void zms_http_ts_routes_register(void)
{
    zms_http_route_register(&k_http_ts_route);
}
