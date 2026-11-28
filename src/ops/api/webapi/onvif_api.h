/**
 * @file onvif_api.h
 * @brief /index/api/onvif/* 的（异步）处理入口；仅供 webapi 内部使用。
 */
#ifndef ZCS_SRC_OPS_API_WEBAPI_ONVIF_API_H
#define ZCS_SRC_OPS_API_WEBAPI_ONVIF_API_H

#include "zcs/ops/api/webapi/webapi.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_onvif_api_req {
    /** "discover" / "probe" / "add" / "del" / "list" / "ptz" */
    const char *endpoint;
    const char *addr;
    const char *user;
    const char *pass;
    const char *profile;
    const char *vhost;
    const char *app;
    const char *stream;
    const char *key;
    int timeout_ms;
    float x;
    float y;
    float z;
    int stop;
} zcs_onvif_api_req;

/**
 * 处理 /index/api/onvif/*。
 * @return 1=已同步写 body（*http_status 有效）；
 *         0=已提交异步任务（稍后回包，body 不写）；
 *        -1=未识别（交由调用方回 404）
 */
int zcs_webapi_onvif_handle(zn_tcp_session *tcp, const zcs_web_api_opts *opts,
                            const zcs_onvif_api_req *req, int *http_status, char *body,
                            size_t body_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_OPS_API_WEBAPI_ONVIF_API_H */
