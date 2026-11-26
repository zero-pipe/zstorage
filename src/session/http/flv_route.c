/**
 * @file flv_route.c
 * @brief HTTP-FLV 路由注册。
 */
#include "zms/protocol/http/route.h"
#include "session/http/session_internal.h"
#include "zms/core/stream/hub.h"
#include "zn/util/log.h"
#include <string.h>

static zms_media_source *http_find_play_src(const zms_http_request *req, const char *app,
                                           const char *stream)
{
    char vhost[ZMS_VHOST_MAX];

    zms_http_request_vhost(req, vhost, sizeof(vhost));
    return zms_media_source_find_for_play_ex(ZMS_SCHEMA_RTMP, vhost, app, stream);
}



static int http_flv_route_match(const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        return 0;
    }
    src = http_find_play_src(req, app, stream);
    return src && !zms_media_source_is_vod(src) && zms_media_source_use_gop_queue_play(src);
}

static void http_flv_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || zms_media_source_is_vod(src) || !zms_media_source_use_gop_queue_play(src)) {
        zn_warn("HTTP-FLV live 404: app=%s stream=%s src=%p vod=%d gop_play=%d", app, stream,
                 (void *)src, src ? zms_media_source_is_vod(src) : 0,
                 src ? zms_media_source_use_gop_queue_play(src) : 0);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    zms_http_live_flv_start(hs, src, app, stream,
                            zms_http_req_is_ws_upgrade(req)
                                ? zms_http_req_header(req, "Sec-WebSocket-Key")
                                : NULL);
}

static int http_flv_fallback_route_match(const zms_http_request *req)
{
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    if (zms_http_route_parse_live_ts_path(req->path, app, stream)) {
        return 0;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    return app[0] && stream[0];
}

static void http_flv_fallback_route_handle(zms_http_session *hs, const zms_http_request *req)
{
    zms_media_source *src;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zms_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zms_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zms_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || (!zms_media_source_use_gop_queue_play(src) && !zms_media_source_is_vod(src))) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        zms_http_session_stop_stream(hs);
    }
    if (zms_media_source_is_vod(src)) {
        zms_http_vod_flv_start(hs, src, app, stream, seek_ms, zms_http_req_header(req, "Range"));
    } else {
        zms_http_live_flv_start(hs, src, app, stream,
                            zms_http_req_is_ws_upgrade(req)
                                ? zms_http_req_header(req, "Sec-WebSocket-Key")
                                : NULL);
    }
}

static const zms_http_route_ops k_http_flv_route = {
    .name = "http-flv-live",
    .match = http_flv_route_match,
    .handle = http_flv_route_handle,
};

static const zms_http_route_ops k_http_flv_fallback_route = {
    .name = "http-flv-fallback",
    .match = http_flv_fallback_route_match,
    .handle = http_flv_fallback_route_handle,
};

void zms_http_flv_routes_register(void)
{
    zms_http_route_register(&k_http_flv_route);
    zms_http_route_register(&k_http_flv_fallback_route);
}
