/**
 * @file flv_tag_demuxer.h
 * @brief FLV tag 解复用状态机。
 */
#ifndef ZMS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H
#define ZMS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H

/**
 * FLV tag body → ES（Annex-B H.264 / AAC raw），封装 vendored libflv/flv-demuxer。
 * 输入为已拆好的 tag；字节流拆包见 flv_tag_framer.h。
 */
#include "zms/zms_export.h"
#include "zms/core/frame.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_flv_tag_demuxer zms_flv_tag_demuxer;

typedef void (*zms_flv_tag_demux_frame_cb)(const zms_frame *frame, void *user);

typedef struct zms_flv_tag_demuxer_opts {
    zms_flv_tag_demux_frame_cb on_frame;
    void *user;
} zms_flv_tag_demuxer_opts;

/** 创建 FLV tag 解复用器；on_frame 必填。@return 实例，opts/on_frame 缺失或内存不足返回 NULL。 */
ZMS_API zms_flv_tag_demuxer *zms_flv_tag_demuxer_create(const zms_flv_tag_demuxer_opts *opts);
/** 销毁解复用器及其内部 flv-demuxer 状态。 */
ZMS_API void zms_flv_tag_demuxer_destroy(zms_flv_tag_demuxer *d);

/** @param type_id 8/9/18；data 为 RTMP/FLV tag body（无 Audio/VideoTagHeader） */
ZMS_API zn_err_t zms_flv_tag_demuxer_input(zms_flv_tag_demuxer *d, uint8_t type_id,
                                            const void *data, size_t len, uint32_t tag_dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H */
