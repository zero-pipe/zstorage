/**
 * @file msg.h
 * @brief 协议消息的统一外壳（零拷贝视图）。
 *
 * 设计目的：让「协议字段」成为消息里的一项键值，而不是结构体里的一个固定成员。
 * 新增一个字段 = 解析器往 hdr 里追加一项，业务按名字读取，不需要改结构体。
 *
 * 约定：
 *   - method/url/hdr/body 全部是**指向接收缓冲的视图**，本模块不拷贝、不释放；
 *     其生命周期与来源缓冲一致（splitter 的报文缓冲）。
 *   - body 的具体格式（SDP 行式 / XML / AMF）由各自的文档助手惰性解析，
 *     doc/doc_owner 留给上层挂接，本模块不解释其内容。
 *   - hdr 存的是**协议元数据**：HTTP 头、RTSP 头、SIP 头、查询参数等，
 *     具体叫什么由各协议解析器决定。
 */
#ifndef ZMS_MESSAGE_MSG_H
#define ZMS_MESSAGE_MSG_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单条消息可容纳的头部项上限（HTTP 实测 <20，RTSP <10，留足余量）。 */
#define ZMS_MSG_HDR_MAX 48

typedef enum zms_msg_kind {
    ZMS_MSG_KIND_REQUEST = 0,
    ZMS_MSG_KIND_RESPONSE = 1,
} zms_msg_kind;

/** 键值视图：name/value 指向原缓冲。 */
typedef struct zms_kv {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} zms_kv;

/** 消息外壳。 */
typedef struct zms_msg {
    zms_msg_kind kind;
    const char *method; /**< 请求方法/命令名；响应消息为 NULL */
    size_t method_len;
    const char *url; /**< 请求 URL（可能带查询串）；可为 NULL */
    size_t url_len;
    int status; /**< 响应状态码；请求消息为 0 */
    const char *reason; /**< 响应原因短语；可为 NULL */
    size_t reason_len;
    zms_kv hdr[ZMS_MSG_HDR_MAX];
    int hdr_n;
    const char *body;
    size_t body_len;
    /** 惰性解析的文档（SDP/XML/AMF），由具体协议的文档助手挂接。 */
    void *doc;
    void *doc_owner;
    /** 本消息自有的堆字符串（键名/值/URL 等），由 zms_msg_own_copy 登记。 */
    char **owned;
    int owned_n;
    int owned_cap;
} zms_msg;

/** 复位消息（不释放任何外部缓冲）。 */
ZMS_API void zms_msg_init(zms_msg *m);

/**
 * 把 s 复制成本消息自有的 NUL 结尾字符串，返回指向副本的视图。
 * 用于解析器：原始报文缓冲是临时的（栈或会被复用），而消息要长期可读时。
 * @return 副本指针；内存不足返回 NULL
 */
ZMS_API const char *zms_msg_own_copy(zms_msg *m, const char *s, size_t len);

/** 释放 zms_msg_own_copy 分配的全部字符串并复位消息（不碰 doc_owner）。 */
ZMS_API void zms_msg_fini(zms_msg *m);

/** 设置请求方法/命令名（存视图、去首尾空白，不拷贝）；成功返回 0。 */
ZMS_API int zms_msg_set_method(zms_msg *m, const char *method, size_t len);
/** 设置请求 URL（存视图，不拷贝）；成功返回 0。 */
ZMS_API int zms_msg_set_url(zms_msg *m, const char *url, size_t len);
/** 设置响应状态码与原因短语，并把消息 kind 置为 RESPONSE；reason 存视图。 */
ZMS_API int zms_msg_set_status(zms_msg *m, int status, const char *reason, size_t len);
/** 设置消息体（存视图，不拷贝；body 为 NULL 时长度置 0）。 */
ZMS_API int zms_msg_set_body(zms_msg *m, const char *body, size_t len);
/** 追加一项头部/元数据（会去掉首尾空白与行尾 CRLF；满了返回 -1）。 */
ZMS_API int zms_msg_add_hdr(zms_msg *m, const char *name, size_t name_len, const char *value,
                            size_t value_len);

/**
 * 按名字查头部（大小写不敏感，返回首个匹配）。
 * @return NULL 表示没有该字段
 */
ZMS_API const zms_kv *zms_msg_find(const zms_msg *m, const char *name);

/**
 * 取头部值并拷进调用方缓冲（NUL 结尾）。
 * 注意：头值是**视图**，不保证 NUL 结尾，所以只能拷贝，不能直接当 C 字符串用。
 * @return 0 找到；-1 未找到或超长（out 置空串）
 */
ZMS_API int zms_msg_get_copy(const zms_msg *m, const char *name, char *out, size_t cap);

/**
 * 取整数值；找不到用 def。
 * @return 0 找到并解析成功；-1 未找到或非法（*out 写 def）
 */
ZMS_API int zms_msg_get_int(const zms_msg *m, const char *name, int64_t def, int64_t *out);

/** 取布尔：1/true/yes 为真，0/false/no 为假，其它用 def。 */
ZMS_API int zms_msg_get_bool(const zms_msg *m, const char *name, int def);

/** URL 的查询串视图（'?' 之后，不含 '?'）；没有则返回 NULL 且 *len=0。 */
ZMS_API const char *zms_msg_query(const zms_msg *m, size_t *len);

/**
 * 从 URL 查询串取一项（做 %XX / '+' 解码）。
 * @return 0 找到；-1 未找到或超长
 */
ZMS_API int zms_msg_query_get(const zms_msg *m, const char *name, char *out, size_t cap);
/**
 * 从 URL 查询串取整数（先做 %XX/'+' 解码再 atoll）。
 * @return 0 找到并解析成功；-1 未找到或非法（*out 写 def）
 */
ZMS_API int zms_msg_query_get_int(const zms_msg *m, const char *name, int64_t def, int64_t *out);

/** 把 kv 的名字/值拷进调用方缓冲（保证 NUL 结尾）。超长返回 -1。 */
ZMS_API int zms_kv_copy_name(const zms_kv *kv, char *out, size_t cap);
/** 拷贝 kv 的值到 out（NUL 结尾）；容量不足（value_len>=cap）返回 -1 且 out 置空。 */
ZMS_API int zms_kv_copy_value(const zms_kv *kv, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MESSAGE_MSG_H */
