/**
 * @file parser.h
 * @brief RTSP 消息解析与构造。
 */
#ifndef ZCS_PROTOCOL_RTSP_PARSER_H
#define ZCS_PROTOCOL_RTSP_PARSER_H

#include "zcs/message/msg.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "rtsp.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_RTSP_KEY_MAX 64
/** 单条头的值上限（协议未规定；超过视为非法报文）。 */
#define ZCS_RTSP_VAL_MAX 4096
#define ZCS_RTSP_LINE_MAX 2048
/** 头部项上限（统一外壳定义）。 */
#define ZCS_RTSP_HDR_MAX ZCS_MSG_HDR_MAX

/**
 * 一条 RTSP 消息。
 *
 * 方法、状态码、版本是 RTSP 专有字段；URL、头字段、body 全部放在统一外壳
 * msg 里（见 zcs/message/msg.h），与 HTTP/SIP 等协议保持同一套访问方式。
 * 头字段与 URL 是消息**自有**的堆副本（原报文缓冲是临时的）。
 */
typedef struct zcs_rtsp_message {
    zcs_msg msg;
    zcs_rtsp_method method;
    int status_code;
    char version[16];
} zcs_rtsp_message;

static inline int zcs_rtsp_message_is_response(const zcs_rtsp_message *m)
{
    return m && m->msg.kind == ZCS_MSG_KIND_RESPONSE;
}
static inline const char *zcs_rtsp_message_url(const zcs_rtsp_message *m)
{
    return m ? m->msg.url : NULL;
}
static inline const char *zcs_rtsp_message_body(const zcs_rtsp_message *m)
{
    return m ? m->msg.body : NULL;
}
static inline size_t zcs_rtsp_message_body_len(const zcs_rtsp_message *m)
{
    return m ? m->msg.body_len : 0;
}

/** 仅解析头（至 \\r\\r\n\\r\\r\n），不含 body */
/** 释放消息里堆上的头字段（可重复调用）；解析新消息前会自动调用。 */
ZCS_API void zcs_rtsp_message_clear(zcs_rtsp_message *msg);

/**
 * 仅解析头（至空行），不含 body；解析前会清空 msg 里旧的头副本。
 * @return ZN_OK 成功；ZN_ERR_AGAIN 头部不完整（需再等更多字节）
 */
ZCS_API zn_err_t zcs_rtsp_message_parse_header(const char *data, size_t len,
                                                zcs_rtsp_message *msg);
/**
 * 解析完整消息（头 + 按 Content-Length 的 body）。
 * @param consumed 可为 NULL；成功时写入本次消费的字节总数
 * @return ZN_OK；ZN_ERR_AGAIN 数据尚未收全（头部或 body 不完整）
 */
ZCS_API zn_err_t zcs_rtsp_message_parse(const char *data, size_t len, zcs_rtsp_message *msg,
                                         size_t *consumed);
/** 按名取头字段值；@return 借用的字符串（归属 msg），无此头返回 NULL。 */
ZCS_API const char *zcs_rtsp_message_get(const zcs_rtsp_message *msg, const char *key);
/** 解析 Content-Length 头；缺失或非正时返回 0。 */
ZCS_API int zcs_rtsp_message_content_length(const zcs_rtsp_message *msg);
/**
 * 构造 RTSP 请求报文文本（自动带 CSeq/User-Agent）。
 * @param extra_headers 额外头（须含完整 CRLF），可为 NULL
 * @param out_len       可为 NULL；成功时写入总长度
 * @return ZN_OK；cap 不足返回 ZN_ERR_BUFFER_TOO_SMALL；参数非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_rtsp_message_build_request(char *out, size_t cap, zcs_rtsp_method method,
                                                 const char *url, unsigned cseq,
                                                 const char *session, const char *extra_headers,
                                                 const char *body, size_t body_len,
                                                 size_t *out_len);
/**
 * 构造 RTSP 响应报文文本。
 * @param status/reason 状态码与原因短语（reason 为 NULL 时用默认）
 * @param out_len 可为 NULL；成功时写入总长度
 * @return ZN_OK；cap 不足 ZN_ERR_BUFFER_TOO_SMALL；参数非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_rtsp_message_build_response(char *out, size_t cap, int status,
                                                  const char *reason, unsigned cseq,
                                                  const char *session, const char *extra_headers,
                                                  const char *body, size_t body_len,
                                                  size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_PARSER_H */
