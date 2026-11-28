/**
 * @file sha1.h
 * @brief SHA-1 摘要。
 */
#ifndef ZCS_UTIL_SHA1_H
#define ZCS_UTIL_SHA1_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_sha1_ctx {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} zcs_sha1_ctx;

/** 初始化摘要上下文（起算新一次 SHA-1）。 */
ZCS_API void zcs_sha1_init(zcs_sha1_ctx *ctx);
/** 追加数据（可多次调用，流式累计）。 */
ZCS_API void zcs_sha1_update(zcs_sha1_ctx *ctx, const void *data, size_t len);
/** 结束计算并输出 20 字节摘要；之后 ctx 不应再用，除非重新 init。 */
ZCS_API void zcs_sha1_final(zcs_sha1_ctx *ctx, uint8_t digest[20]);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_UTIL_SHA1_H */
