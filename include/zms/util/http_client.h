/**
 * @file http_client.h
 * @brief 极简同步 HTTP 客户端（http://，供 ONVIF SOAP、webhook 等控制面使用）。
 *
 * 特点：
 *   - 无第三方依赖，基于平台 socket（阻塞 + 超时）；
 *   - 仅实现 HTTP/1.1 的 POST/GET 与 Content-Length/chunked 响应解析；
 *   - 调用方负责在独立线程/任务中调用（会阻塞）。
 */
#ifndef ZMS_UTIL_HTTP_CLIENT_H
#define ZMS_UTIL_HTTP_CLIENT_H

#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_req {
    /** 仅支持 http://host[:port]/path */
    const char *url;
    /** GET/POST；NULL 默认 POST */
    const char *method;
    /** 可选，如 application/soap+xml; charset=utf-8 */
    const char *content_type;
    const char *body;
    size_t body_len;
    /** 额外请求头，每行以 \r\n 结尾；可为 NULL */
    const char *extra_headers;
    /** 连接/收发超时（毫秒），<=0 用默认 10000 */
    int timeout_ms;
} zms_http_req;

typedef struct zms_http_resp {
    /** HTTP 状态码；传输/解析失败为 -1 */
    int status;
    /** malloc 的响应体（已 NUL 结尾），可能为 NULL */
    char *body;
    size_t body_len;
    /** 响应头原文（malloc，含结尾空行；便于取 WWW-Authenticate） */
    char *headers;
    /** 0=成功；<0 传输/解析错误 */
    int err;
} zms_http_resp;

/**
 * 同步执行一次 HTTP 请求，结果写入 resp。resp 的 body/headers 为 malloc，需用 resp_free 释放。
 * 调用会阻塞，必须放在独立线程/任务中。
 * @return 0 表示已收到并解析响应（具体结果看 resp->status / resp->err）；<0 传输/解析失败。
 */
ZMS_API int zms_http_exec(const zms_http_req *req, zms_http_resp *resp);
/** 释放 resp 内的 body/headers 并清零结构；对 NULL 安全。 */
ZMS_API void zms_http_resp_free(zms_http_resp *resp);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_HTTP_CLIENT_H */
