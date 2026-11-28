/**
 * @file message_framer.h
 * @brief HTTP 消息组帧。
 */
#ifndef ZCS_PROTOCOL_HTTP_MESSAGE_FRAMER_H
#define ZCS_PROTOCOL_HTTP_MESSAGE_FRAMER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_message_framer zcs_http_message_framer;

/**
 * 收到完整 HTTP 头（含末尾 \r\n\r\n 之前部分）。
 * @return 后续 body 长度语义（HTTP 分块请求体解析）：
 *   >0  固定长度 body，收齐后一次调用 on_content
 *   0   无 body 或仍等待更多头数据
 *   <0  不定长 body，剩余数据分次 on_content
 */
typedef intptr_t (*zcs_http_message_framer_on_header_cb)(const char *header, size_t header_len,
                                                         void *user);

typedef void (*zcs_http_message_framer_on_content_cb)(const char *data, size_t len, void *user);

/**
 * 查找当前缓冲区内一个完整逻辑包末尾（返回尾后第一个字节指针）。
 * NULL 表示数据不足；默认实现查找 \r\n\r\n。
 */
typedef const char *(*zcs_http_message_framer_search_tail_cb)(const char *data, size_t len,
                                                              void *user);

typedef struct zcs_http_message_framer_opts {
    zcs_http_message_framer_on_header_cb on_header;
    zcs_http_message_framer_on_content_cb on_content;
    void *user;
    /** 0 表示默认 4MB */
    size_t max_cache_size;
    /** 可选，RTSP 等协议用于交织包 */
    zcs_http_message_framer_search_tail_cb on_search_tail;
} zcs_http_message_framer_opts;

/** 默认 HTTP/RTSP 文本包尾（\r\n\r\n 之后） */
ZCS_API const char *zcs_http_message_search_tail(const char *data, size_t len);

/**
 * 创建组帧器：按 \r\n\r\n（或自定义 on_search_tail）切头，再按 Content-Length/chunked 切体。
 * max_cache_size 为 0 时默认 4MB；缓存超限则复位并返回 BUFFER_TOO_SMALL。
 * @return 组帧器；分配失败返回 NULL（用 destroy 释放）。
 */
ZCS_API zcs_http_message_framer *
zcs_http_message_framer_create(const zcs_http_message_framer_opts *opts);
/** 销毁组帧器与内部剩余缓冲；s 可为 NULL。 */
ZCS_API void zcs_http_message_framer_destroy(zcs_http_message_framer *s);
/** 复位：清空剩余缓冲与已解析的 body 长度（丢弃半包状态）。 */
ZCS_API void zcs_http_message_framer_reset(zcs_http_message_framer *s);
/**
 * 喂入一段字节，内部拼包并在完整时通过 on_header/on_content 回调。
 * @return ZN_OK 成功；缓存超限 BUFFER_TOO_SMALL；内存不足 NOMEM；参数非法 INVALID。
 */
ZCS_API zn_err_t zcs_http_message_framer_input(zcs_http_message_framer *s, const void *data,
                                                size_t len);

/** 当前内部剩余（尚未消费）字节数。 */
ZCS_API size_t zcs_http_message_framer_remain_size(const zcs_http_message_framer *s);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_HTTP_MESSAGE_FRAMER_H */
