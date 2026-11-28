#ifndef ZCS_MEDIA_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H
#define ZCS_MEDIA_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H

/**
 * @file mpegps_demuxer.h
 * @brief MPEG-PS ES 解复用（GB28181 RTP/PS 接入路径）。
 */
#include "zcs/media/container/frame_callback.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_mpegps_demuxer zcs_mpegps_demuxer;

typedef void (*zcs_mpegps_h264_ps_fn)(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                      size_t pps_len, void *user);

typedef struct zcs_mpegps_demuxer_opts {
    zcs_payload_frame_cb on_frame;
    zcs_mpegps_h264_ps_fn on_h264_ps;
    void *user;
} zcs_mpegps_demuxer_opts;

/**
 * 创建 MPEG-PS 解复用器（GB28181 PS 流）。
 * opts->on_frame 必填；on_h264_ps 可选，用于回传嗅探到的 SPS/PPS。
 * @return 解复用器；缺参或分配失败返回 NULL（用 destroy 释放）。
 */
ZCS_API zcs_mpegps_demuxer *zcs_mpegps_demuxer_create(const zcs_mpegps_demuxer_opts *opts);
/** 销毁解复用器及底层 PS 解复用器；d 可为 NULL。 */
ZCS_API void zcs_mpegps_demuxer_destroy(zcs_mpegps_demuxer *d);

/** 喂入重组好的 PS 字节流（来自 RTP/PS payload）。 */
ZCS_API zn_err_t zcs_mpegps_demuxer_feed(zcs_mpegps_demuxer *d, const uint8_t *data, size_t len);
/** 冲刷解复用器内部状态（当前为空实现，恒返回 ZN_OK）。 */
ZCS_API zn_err_t zcs_mpegps_demuxer_flush(zcs_mpegps_demuxer *d);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_MPEGPS_MPEGPS_DEMUXER_H */
