/**
 * @file hex_decode.h
 * @brief 十六进制解码。
 */
#ifndef ZCS_UTIL_HEX_DECODE_H
#define ZCS_UTIL_HEX_DECODE_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析十六进制字符串为字节序列（忽略空白与非十六进制字符；每两字符一字节）。
 * @param out     输出缓冲
 * @param cap     输出容量（字节）
 * @param out_len 输出实际解码字节数，可为 NULL
 * @return ZN_OK 成功；缓冲不足返回 BUFFER_TOO_SMALL；参数非法返回 INVALID。
 */
ZCS_API zn_err_t zcs_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_UTIL_HEX_DECODE_H */
