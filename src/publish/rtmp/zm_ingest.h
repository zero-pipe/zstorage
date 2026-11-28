/**
 * @file zm_ingest.h
 * @brief RTMP tag → zm FLV 入站适配。
 */
#ifndef ZCS_SRC_PUBLISH_RTMP_ZM_INGEST_H
#define ZCS_SRC_PUBLISH_RTMP_ZM_INGEST_H

#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_rtmp_zm_ingest zcs_rtmp_zm_ingest;

/** 为 RTMP publish 创建 ZM FLV tag-body 解包器。 */
zcs_rtmp_zm_ingest *zcs_rtmp_zm_ingest_create(zcs_live_ingest *ingest);
void zcs_rtmp_zm_ingest_destroy(zcs_rtmp_zm_ingest *z);

/** type: 8-audio / 9-video / 18-script。 */
zn_err_t zcs_rtmp_zm_ingest_input_tag(zcs_rtmp_zm_ingest *z, int type, const void *body,
                                        size_t len, uint32_t dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_PUBLISH_RTMP_ZM_INGEST_H */
