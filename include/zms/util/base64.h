/**
 * @file base64.h
 * @brief Base64 编解码（RFC 4648 标准字母表）。
 *
 * 此前该算法在 6 个模块各有一份实现（RTSP SDP、WebRTC SDP、ONVIF SOAP、
 * 拉流代理、WebSocket 握手），边界处理与返回值约定各不相同。统一到这里，
 * 新代码不要再自写。
 */
#ifndef ZMS_UTIL_BASE64_H
#define ZMS_UTIL_BASE64_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 编码。
 * @param out     输出缓冲（写入以 NUL 结尾的字符串）
 * @param cap     输出容量（含结尾 NUL 的空间）
 * @param out_len 实际写入字符数（不含结尾 NUL），可为 NULL
 * @return ZN_OK；ZN_ERR_BUFFER_TOO_SMALL 表示 cap 不足（此时 out 内容不可用）
 */
ZMS_API zn_err_t zms_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap,
                                   size_t *out_len);

/**
 * 解码。忽略空格/换行/tab 与 '=' 填充；遇到非法字符立即停止。
 * @param out     输出字节缓冲
 * @param cap     输出容量（字节）
 * @param out_len 实际解出的字节数，可为 NULL
 * @return ZN_OK；ZN_ERR_INVALID 参数非法；ZN_ERR_BUFFER_TOO_SMALL 输出截断
 *         （已解出的部分仍有效，out_len 返回已写入长度）
 */
ZMS_API zn_err_t zms_base64_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len);

/** 解码后的最大字节数（不含 NUL），用于分配缓冲。 */
ZMS_API size_t zms_base64_decoded_max(size_t in_len);

/** 编码后的最大字符数（不含 NUL），用于分配缓冲。 */
ZMS_API size_t zms_base64_encoded_max(size_t in_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_BASE64_H */
