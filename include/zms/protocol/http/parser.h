/**
 * @file parser.h
 * @brief HTTP 请求/响应解析。
 */
#ifndef ZMS_PROTOCOL_HTTP_PARSER_H
#define ZMS_PROTOCOL_HTTP_PARSER_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_HTTP_METHOD_MAX 32
#define ZMS_HTTP_URL_MAX 2048
#define ZMS_HTTP_PROTO_MAX 32
#define ZMS_HTTP_KEY_MAX 128
#define ZMS_HTTP_VAL_MAX 4096
#define ZMS_HTTP_HDR_MAX 64

/**
 * 一个头字段。
 * value 在堆上（解析时按实际长度分配，上限 ZMS_HTTP_VAL_MAX）：
 * 每个连接最多 ZMS_HTTP_HDR_MAX 个头，若按定长数组存放会放大成数百 KB。
 */
typedef struct zms_http_header {
    char key[ZMS_HTTP_KEY_MAX];
    char *value;
} zms_http_header;

typedef struct zms_http_parser {
    char method[ZMS_HTTP_METHOD_MAX];
    char url[ZMS_HTTP_URL_MAX];
    char params[ZMS_HTTP_URL_MAX];
    char protocol[ZMS_HTTP_PROTO_MAX];
    zms_http_header headers[ZMS_HTTP_HDR_MAX];
    size_t header_count;
    char *content;
    size_t content_len;
    size_t content_cap;
    int is_response;
} zms_http_parser;

/** 清零解析器结构（不释放）；对 NULL 安全。 */
ZMS_API void zms_http_parser_init(zms_http_parser *p);
/** 释放堆上的头值与 body（并清零）；用于复用前或销毁前。对 NULL 安全。 */
ZMS_API void zms_http_parser_clear(zms_http_parser *p);

/** 解析完整报文（头 + body，buf 内连续） */
ZMS_API zn_err_t zms_http_parser_parse(zms_http_parser *p, const char *buf, size_t size);

/** 仅解析请求/响应头（不含 body） */
ZMS_API zn_err_t zms_http_parser_parse_header(zms_http_parser *p, const char *buf, size_t size);

/** 设置 body（拷贝）；供 packet_splitter on_content 后调用 */
ZMS_API zn_err_t zms_http_parser_set_content(zms_http_parser *p, const char *data, size_t len);

/** 大小写不敏感查找 */
ZMS_API const char *zms_http_parser_header(const zms_http_parser *p, const char *name);

/** url + ?params（无 query 时仅 url） */
ZMS_API const char *zms_http_parser_full_url(const zms_http_parser *p, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_HTTP_PARSER_H */
