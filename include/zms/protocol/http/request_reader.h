/**
 * @file request_reader.h
 * @brief HTTP 请求体读取。
 */
#ifndef ZMS_PROTOCOL_HTTP_REQUEST_READER_H
#define ZMS_PROTOCOL_HTTP_REQUEST_READER_H

#include "zms/message/msg.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 一个 HTTP 请求。
 *
 * msg 是统一外壳（见 zms/message/msg.h）：方法、URL、全部头字段、body 都在里面，
 * 新增要读的字段不再需要改本结构。下面的旧字段在迁移期保留，等价于 msg 的视图：
 *   method/path/body    ← msg.method / msg.url / msg.body
 * 协议专用字段（Host/Range/Sec-WebSocket-Key/Upgrade）不再有成员，
 * 一律用 zms_http_req_header()/zms_http_req_is_ws_upgrade() 按名字取。
 *
 * 生命周期：本结构及其 msg 视图只在下发回调期间有效。
 */
typedef struct zms_http_request {
    zms_msg msg;
    char method[16];
    char path[512];
    const char *body;
    size_t body_len;
} zms_http_request;

/** 取请求头（NUL 结尾；HTTP 头值由解析器堆缓冲持有，可安全当 C 字符串）。 */
ZMS_API const char *zms_http_req_header(const zms_http_request *req, const char *name);

/** 是否 WebSocket 升级请求（Upgrade: websocket）。 */
ZMS_API int zms_http_req_is_ws_upgrade(const zms_http_request *req);

typedef void (*zms_http_on_request_cb)(const zms_http_request *req, void *user);

typedef struct zms_http_request_reader zms_http_request_reader;

typedef struct zms_http_request_reader_opts {
    zms_http_on_request_cb on_request;
    void *user;
} zms_http_request_reader_opts;

/** 创建 HTTP 请求读取器；opts 可为 NULL（无回调时仅做解析）。返回实例，失败返回 NULL。 */
ZMS_API zms_http_request_reader *
zms_http_request_reader_create(const zms_http_request_reader_opts *opts);
/** 释放读取器及内部 framer/parser；对 NULL 安全。 */
ZMS_API void zms_http_request_reader_destroy(zms_http_request_reader *s);
/** 复位读取器状态（丢弃未完成的请求），用于长连接复用；对 NULL 安全。 */
ZMS_API void zms_http_request_reader_reset(zms_http_request_reader *s);
/** 喂入字节流，解析出完整请求时触发 on_request；返回底层 framer 的状态码。 */
ZMS_API zn_err_t zms_http_request_reader_input(zms_http_request_reader *s, const void *data,
                                                size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_HTTP_REQUEST_READER_H */
