/**
 * @file route.h
 * @brief HTTP 路由注册契约：任何模块（HLS/FLV/DASH/VOD/ONVIF/WebAPI/WebRTC）
 *        通过它把自己的前缀匹配与处理函数挂到 HTTP 入口上。
 *
 * 放在公开头里是为了让注册方不反向依赖会话层：注册方只见到请求视图
 * （`zcs_http_request`）与两条回调，不包含 HTTP 会话内部结构。
 */
#ifndef ZCS_PROTOCOL_HTTP_ROUTE_H
#define ZCS_PROTOCOL_HTTP_ROUTE_H

#include "zcs/protocol/http/request_reader.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_session zcs_http_session;

typedef int (*zcs_http_route_match_fn)(const zcs_http_request *req);
typedef void (*zcs_http_route_handle_fn)(zcs_http_session *sess, const zcs_http_request *req);

typedef struct zcs_http_route_ops {
    const char *name;
    zcs_http_route_match_fn match;
    zcs_http_route_handle_fn handle;
} zcs_http_route_ops;

void zcs_http_route_register(const zcs_http_route_ops *ops);
void zcs_http_routes_register_all(void);
int zcs_http_router_dispatch(zcs_http_session *sess, const zcs_http_request *req);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_HTTP_ROUTE_H */
