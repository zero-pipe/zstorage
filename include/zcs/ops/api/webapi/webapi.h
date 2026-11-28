/**
 * @file webapi.h
 * @brief Web API。
 */
#ifndef ZCS_OPS_API_WEBAPI_WEBAPI_H
#define ZCS_OPS_API_WEBAPI_WEBAPI_H

/**
 * HTTP REST API。
 */
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/net/tcp_server.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 对齐 server/WebApi.h ApiErr */
typedef enum zcs_api_err {
    ZCS_API_SUCCESS = 0,
    ZCS_API_OTHER_FAILED = -1,
    ZCS_API_AUTH_FAILED = -100,
    ZCS_API_INVALID_ARGS = -300,
    ZCS_API_NOT_FOUND = -500,
} zcs_api_err;

typedef struct zcs_config zcs_config;

typedef struct zcs_web_api_opts {
    const char *api_secret;
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const zcs_config *cfg;
} zcs_web_api_opts;

/**
 * 处理 /index/api/* 请求。
 * @param method GET/POST
 * @param path_with_query 含 query 的 path，如 /index/api/media/list?app=live
 * @return 写入 body 的字节数；*http_status 为 HTTP 状态码
 */
ZCS_API size_t zcs_web_api_handle(zn_tcp_session *tcp, const char *method,
                                  const char *path_with_query, const zcs_web_api_opts *opts,
                                  int *http_status, char *body, size_t body_cap);

/** 记录单调时钟启动时间，供 health/statistic 计算 uptime（服务启动时调用一次）。 */
ZCS_API void zcs_web_api_note_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_OPS_API_WEBAPI_WEBAPI_H */
