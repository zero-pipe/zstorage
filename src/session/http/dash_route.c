/**
 * @file dash_route.c
 * @brief DASH 路由注册。
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



static int dash_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char file[128];

    return req && zcs_http_route_parse_dash_path(req->path, app, stream, file, sizeof(file));
}

static void dash_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char file[128];

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zcs_http_route_parse_dash_path(req->path, app, stream, file, sizeof(file))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || zcs_media_source_is_vod(src) || !src->gop_queue) {
        zn_warn("DASH live 404: no live source app=%s stream=%s src=%p vod=%d ring=%p", app,
                 stream, (void *)src, src ? zcs_media_source_is_vod(src) : 0,
                 src ? (void *)src->gop_queue : NULL);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zcs_http_live_dash_handle(hs, src, app, stream, file);
}

static const zcs_http_route_ops k_dash_route = {
    .name = "dash",
    .match = dash_route_match,
    .handle = dash_route_handle,
};

void zcs_http_dash_routes_register(void)
{
    zcs_http_route_register(&k_dash_route);
}
