#ifndef ZCS_EGRESS_MPEGTS_MPEGTS_EGRESS_H
#define ZCS_EGRESS_MPEGTS_MPEGTS_EGRESS_H

/**
 * @file mpegts_egress.h
 * @brief 直播 MPEG-TS 出站（连续 188 字节 TS，供 SRT/HTTP-TS）。
 */
struct zcs_media_source;
struct zcs_egress_source;
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_mpegts_egress zcs_mpegts_egress;

/** 创建绑定媒体源 + egress_source 读者的直播 TS 出站。 */
ZCS_API zcs_mpegts_egress *zcs_mpegts_egress_create(struct zcs_media_source *src,
                                                    struct zcs_egress_source *play);
/** 销毁 muxer 与内部缓冲；对 NULL 安全。 */
ZCS_API void zcs_mpegts_egress_destroy(zcs_mpegts_egress *m);

/**
 * 从 ring mux 并复制发送队列最多 @a cap 字节（优先 1316）。
 * @return 写入字节数，无数据/错误则 <=0
 */
ZCS_API int zcs_mpegts_egress_next(zcs_mpegts_egress *m, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_MPEGTS_MPEGTS_EGRESS_H */
