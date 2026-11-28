#ifndef ZCS_MEDIA_CONTAINER_DEMUX_PIPELINE_H
#define ZCS_MEDIA_CONTAINER_DEMUX_PIPELINE_H

/**
 * @file demux_pipeline.h
 * @brief 容器 + payload 解复用管线 → zcs_frame 回调。
 *
 * 公共接口保持精简：具体 mpegts/flv demuxer 实现在 .c 中。
 */
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/media/container/frame_callback.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_demux_pipeline zcs_demux_pipeline;

/** 可选 H.264 参数集回调（MPEG-TS 入站）。 */
typedef void (*zcs_demux_pipeline_h264_ps_fn)(const uint8_t *sps, size_t sps_len,
                                             const uint8_t *pps, size_t pps_len, void *user);

typedef struct zcs_demux_pipeline_opts {
    zcs_container_id container;
    zcs_payload_frame_cb on_frame;
    zcs_demux_pipeline_h264_ps_fn on_mpegts_h264_ps;
    void *user;
} zcs_demux_pipeline_opts;

/**
 * 创建解复用管线（容器 demux + payload demux → on_frame 回调）。
 * @param opts container 必须在容器注册表中存在，on_frame 必填；on_mpegts_h264_ps 可选。
 * @return 管线实例；参数非法或子 demuxer 创建失败返回 NULL。用 destroy 释放。
 */
ZCS_API zcs_demux_pipeline *zcs_demux_pipeline_create(const zcs_demux_pipeline_opts *opts);
/** 释放管线及内部子 demuxer；对 NULL 安全。 */
ZCS_API void zcs_demux_pipeline_destroy(zcs_demux_pipeline *p);

/**
 * 喂入容器字节流做解复用（MPEG-TS 容器）。FLV_TAG 容器请改用 input_flv_tag。
 * @return ZN_OK 或容器解析错误码。
 */
ZCS_API zn_err_t zcs_demux_pipeline_feed(zcs_demux_pipeline *p, const uint8_t *buf, size_t len);
/** 冲刷解复用器残余数据（仅 MPEG-TS 有效）；对 NULL 安全。 */
ZCS_API void zcs_demux_pipeline_flush(zcs_demux_pipeline *p);
/**
 * 直接投递一条已切好的 FLV tag（跳过容器 framing）。
 * @param track_index 预留参数，当前实现忽略
 * @return ZN_OK 或容器 input_tag 错误码；不支持 input_tag 的容器返回 ZN_ERR_INVALID。
 */
ZCS_API zn_err_t zcs_demux_pipeline_input_flv_tag(zcs_demux_pipeline *p, int track_index,
                                                   uint8_t type_id, const uint8_t *body, size_t len,
                                                   uint32_t tag_dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_DEMUX_PIPELINE_H */
