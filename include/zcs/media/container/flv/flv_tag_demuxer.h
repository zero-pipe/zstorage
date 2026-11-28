/**
 * @file flv_tag_demuxer.h
 * @brief FLV tag 解复用状态机。
 */
#ifndef ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H
#define ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H

/**
 * FLV tag body → ES（Annex-B H.264 / AAC raw），封装 vendored libflv/flv-demuxer。
 * 输入为已拆好的 tag；字节流拆包见 flv_tag_framer.h。
 */
#include "zcs/zcs_export.h"
#include "zcs/core/frame.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_flv_tag_demuxer zcs_flv_tag_demuxer;

typedef void (*zcs_flv_tag_demux_frame_cb)(const zcs_frame *frame, void *user);

typedef struct zcs_flv_tag_demuxer_opts {
    zcs_flv_tag_demux_frame_cb on_frame;
    void *user;
} zcs_flv_tag_demuxer_opts;

/** 创建 FLV tag 解复用器；on_frame 必填。@return 实例，opts/on_frame 缺失或内存不足返回 NULL。 */
ZCS_API zcs_flv_tag_demuxer *zcs_flv_tag_demuxer_create(const zcs_flv_tag_demuxer_opts *opts);
/** 销毁解复用器及其内部 flv-demuxer 状态。 */
ZCS_API void zcs_flv_tag_demuxer_destroy(zcs_flv_tag_demuxer *d);

/** @param type_id 8/9/18；data 为 RTMP/FLV tag body（无 Audio/VideoTagHeader） */
ZCS_API zn_err_t zcs_flv_tag_demuxer_input(zcs_flv_tag_demuxer *d, uint8_t type_id,
                                            const void *data, size_t len, uint32_t tag_dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_DEMUXER_H */
