/**
 * @file gb28181_api.h
 * @brief /index/api/gb28181/* 处理入口（仅供 webapi 内部使用）。
 */
#ifndef ZCS_SRC_OPS_API_WEBAPI_GB28181_API_H
#define ZCS_SRC_OPS_API_WEBAPI_GB28181_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_gb28181_api_req {
    /** status / channels / alarm */
    const char *endpoint;
    const char *channel;
    const char *device;
    const char *cmd;
    const char *call_id;
    const char *app;
    const char *stream;
    const char *start;
    const char *end;
    float pan;
    float tilt;
    float zoom;
    int stop;
    int tcp;
    int priority;
    int method;
    const char *description;
} zcs_gb28181_api_req;

/**
 * @return >0 已写 body；-1 未识别 endpoint。
 */
int zcs_webapi_gb28181_handle(const zcs_gb28181_api_req *req, int *http_status, char *body,
                              size_t body_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_OPS_API_WEBAPI_GB28181_API_H */
