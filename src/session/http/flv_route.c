/**
 * @file flv_route.c
 * @brief HTTP-FLV 路由注册。
 */
#include "zcs/protocol/http/route.h"
#include "session/http/session_internal.h"
#include "zcs/core/stream/hub.h"
#include "zn/util/log.h"
#include <string.h>

static zcs_media_source *http_find_play_src(const zcs_http_request *req, const char *app,
                                           const char *stream)
{
    char vhost[ZCS_VHOST_MAX];

    zcs_http_request_vhost(req, vhost, sizeof(vhost));
    return zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, vhost, app, stream);
}



static int http_flv_route_match(const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        return 0;
    }
    src = http_find_play_src(req, app, stream);
    return src && !zcs_media_source_is_vod(src) && zcs_media_source_use_gop_queue_play(src);
}

static void http_flv_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || zcs_media_source_is_vod(src) || !zcs_media_source_use_gop_queue_play(src)) {
        zn_warn("HTTP-FLV live 404: app=%s stream=%s src=%p vod=%d gop_play=%d", app, stream,
                 (void *)src, src ? zcs_media_source_is_vod(src) : 0,
                 src ? zcs_media_source_use_gop_queue_play(src) : 0);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    zcs_http_live_flv_start(hs, src, app, stream,
                            zcs_http_req_is_ws_upgrade(req)
                                ? zcs_http_req_header(req, "Sec-WebSocket-Key")
                                : NULL);
}

static int http_flv_fallback_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    if (zcs_http_route_parse_live_ts_path(req->path, app, stream)) {
        return 0;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    return app[0] && stream[0];
}

static void http_flv_fallback_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || (!zcs_media_source_use_gop_queue_play(src) && !zcs_media_source_is_vod(src))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    if (zcs_media_source_is_vod(src)) {
        zcs_http_vod_flv_start(hs, src, app, stream, seek_ms, zcs_http_req_header(req, "Range"));
    } else {
        zcs_http_live_flv_start(hs, src, app, stream,
                            zcs_http_req_is_ws_upgrade(req)
                                ? zcs_http_req_header(req, "Sec-WebSocket-Key")
                                : NULL);
    }
}

static const zcs_http_route_ops k_http_flv_route = {
    .name = "http-flv-live",
    .match = http_flv_route_match,
    .handle = http_flv_route_handle,
};

static const zcs_http_route_ops k_http_flv_fallback_route = {
    .name = "http-flv-fallback",
    .match = http_flv_fallback_route_match,
    .handle = http_flv_fallback_route_handle,
};

void zcs_http_flv_routes_register(void)
{
    zcs_http_route_register(&k_http_flv_route);
    zcs_http_route_register(&k_http_flv_fallback_route);
}
