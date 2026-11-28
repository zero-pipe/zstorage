/**
 * @file flv_tag_framer.h
 * @brief FLV tag 组帧。
 */
#ifndef ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_FRAMER_H
#define ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_FRAMER_H

/**
 * FLV 字节流 → 完整 tag 拆包（framing），不解析 ES。
 */
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_flv_tag_framer zcs_flv_tag_framer;

typedef void (*zcs_flv_on_tag)(uint8_t type, const uint8_t *body, size_t len, uint32_t tag_dts_ms,
                               void *user);

/** 创建 FLV tag 组帧器（初始处于文件头解析状态）；分配失败返回 NULL。 */
ZCS_API zcs_flv_tag_framer *zcs_flv_tag_framer_create(void);
/** 释放组帧器及内部 tag 缓冲；对 NULL 安全。 */
ZCS_API void zcs_flv_tag_framer_destroy(zcs_flv_tag_framer *s);
/** 复位解析状态，使其重新从 FLV 文件头开始解析；对 NULL 安全。 */
ZCS_API void zcs_flv_tag_framer_reset(zcs_flv_tag_framer *s);
/**
 * 喂入任意字节片段，切分出完整 tag 时回调 on_tag（body 借用，仅回调期间有效）。
 * 支持跨片段缓冲；单 tag body 上限 4 MB，超限返回 ZN_ERR_BUFFER_TOO_SMALL。
 */
ZCS_API zn_err_t zcs_flv_tag_framer_feed(zcs_flv_tag_framer *s, const uint8_t *data, size_t len,
                                          zcs_flv_on_tag on_tag, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_FRAMER_H */
