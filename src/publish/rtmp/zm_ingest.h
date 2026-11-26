/**
 * @file zm_ingest.h
 * @brief RTMP tag → zm FLV 入站适配。
 */
#ifndef ZMS_SRC_PUBLISH_RTMP_ZM_INGEST_H
#define ZMS_SRC_PUBLISH_RTMP_ZM_INGEST_H

#include "zms/publish/ingest/common/stream_ingest.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtmp_zm_ingest zms_rtmp_zm_ingest;

/** 为 RTMP publish 创建 ZM FLV tag-body 解包器。 */
zms_rtmp_zm_ingest *zms_rtmp_zm_ingest_create(zms_live_ingest *ingest);
void zms_rtmp_zm_ingest_destroy(zms_rtmp_zm_ingest *z);

/** type: 8-audio / 9-video / 18-script。 */
zn_err_t zms_rtmp_zm_ingest_input_tag(zms_rtmp_zm_ingest *z, int type, const void *body,
                                        size_t len, uint32_t dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_PUBLISH_RTMP_ZM_INGEST_H */
