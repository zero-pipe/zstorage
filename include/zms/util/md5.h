/**
 * @file md5.h
 * @brief MD5 摘要。
 */
#ifndef ZMS_UTIL_MD5_H
#define ZMS_UTIL_MD5_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 计算 data 的 MD5，输出原始 16 字节摘要到 digest。 */
ZMS_API void zms_md5(const uint8_t *data, size_t len, uint8_t digest[16]);
/** 小写 hex，至少 33 字节 */
ZMS_API void zms_md5_hex(const uint8_t *data, size_t len, char out[33]);
/** 计算以 NUL 结尾字符串 s 的 MD5，输出 32 字节小写 hex + NUL 到 out。 */
ZMS_API void zms_md5_hex_str(const char *s, char out[33]);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_MD5_H */
