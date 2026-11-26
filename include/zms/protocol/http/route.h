/**
 * @file route.h
 * @brief HTTP 路由注册契约：任何模块（HLS/FLV/DASH/VOD/ONVIF/WebAPI/WebRTC）
 *        通过它把自己的前缀匹配与处理函数挂到 HTTP 入口上。
 *
 * 放在公开头里是为了让注册方不反向依赖会话层：注册方只见到请求视图
 * （`zms_http_request`）与两条回调，不包含 HTTP 会话内部结构。
 */
#ifndef ZMS_PROTOCOL_HTTP_ROUTE_H
#define ZMS_PROTOCOL_HTTP_ROUTE_H

#include "zms/protocol/http/request_reader.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_session zms_http_session;

typedef int (*zms_http_route_match_fn)(const zms_http_request *req);
typedef void (*zms_http_route_handle_fn)(zms_http_session *sess, const zms_http_request *req);

typedef struct zms_http_route_ops {
    const char *name;
    zms_http_route_match_fn match;
    zms_http_route_handle_fn handle;
} zms_http_route_ops;

void zms_http_route_register(const zms_http_route_ops *ops);
void zms_http_routes_register_all(void);
int zms_http_router_dispatch(zms_http_session *sess, const zms_http_request *req);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_HTTP_ROUTE_H */
