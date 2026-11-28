/**
 * @file router.c
 * @brief HTTP 路由注册汇总。
 */
#include "zcs/protocol/http/route.h"
#include <string.h>

#define ZCS_HTTP_ROUTE_MAX 16

static const zcs_http_route_ops *g_routes[ZCS_HTTP_ROUTE_MAX];
static int g_route_count;

void zcs_webapi_http_routes_register(void);
#if defined(ZCS_ENABLE_WEBRTC)
void zcs_webrtc_whep_routes_register(void);
void zcs_webrtc_whip_routes_register(void);
#endif

void zcs_http_route_register(const zcs_http_route_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->handle) {
        return;
    }
    for (i = 0; i < g_route_count; ++i) {
        if (g_routes[i] && strcmp(g_routes[i]->name, ops->name) == 0) {
            g_routes[i] = ops;
            return;
        }
    }
    if (g_route_count < ZCS_HTTP_ROUTE_MAX) {
        g_routes[g_route_count++] = ops;
    }
}

int zcs_http_router_dispatch(zcs_http_session *sess, const zcs_http_request *req)
{
    int i;

    if (!sess || !req) {
        return 0;
    }
    zcs_http_routes_register_all();
    for (i = 0; i < g_route_count; ++i) {
        const zcs_http_route_ops *route = g_routes[i];
        if (route && (!route->match || route->match(req))) {
            route->handle(sess, req);
            return 1;
        }
    }
    return 0;
}

void zcs_http_routes_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
#if defined(ZCS_ENABLE_WEBRTC)
    zcs_webrtc_whep_routes_register();
    zcs_webrtc_whip_routes_register();
#endif
    zcs_webapi_http_routes_register();
}
