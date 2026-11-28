/**
 * @file hls_route.c
 * @brief HLS 路由注册。
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



static int hls_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char hls_file[128];

    return req && zcs_http_route_parse_hls_path(req->path, app, stream, hls_file, sizeof(hls_file));
}

static void hls_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char hls_file[128];

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zcs_http_route_parse_hls_path(req->path, app, stream, hls_file, sizeof(hls_file))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zcs_media_source_is_vod(src)) {
        char vhost[ZCS_VHOST_MAX];
        zcs_http_request_vhost(req, vhost, sizeof(vhost));
        zcs_http_vod_hls_handle(hs, vhost, src, app, stream, hls_file);
    } else {
        zcs_http_live_hls_handle(hs, src, app, stream, hls_file);
    }
}

static const zcs_http_route_ops k_hls_route = {
    .name = "hls",
    .match = hls_route_match,
    .handle = hls_route_handle,
};

void zcs_http_hls_routes_register(void)
{
    zcs_http_route_register(&k_hls_route);
}
