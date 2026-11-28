#ifndef ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_DEMUXER_H
#define ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_DEMUXER_H

/**
 * @file mpegts_demuxer.h
 * @brief MPEG-TS ES 解复用（SRT/HLS 接入路径）。
 */
#include "zcs/zcs_export.h"
#include "zcs/media/container/frame_callback.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_mpegts_demuxer zcs_mpegts_demuxer;

typedef void (*zcs_mpegts_h264_ps_fn)(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                      size_t pps_len, void *user);

typedef struct zcs_mpegts_demuxer_opts {
    zcs_payload_frame_cb on_frame;
    zcs_mpegts_h264_ps_fn on_h264_ps;
    void *user;
} zcs_mpegts_demuxer_opts;

/**
 * 创建 MPEG-TS 解复用器。on_frame 必需，on_h264_ps 可选（嗅探到 H.264 参数集时回调）。
 * @return 解复用器；参数缺失或内存不足返回 NULL
 */
ZCS_API zcs_mpegts_demuxer *zcs_mpegts_demuxer_create(const zcs_mpegts_demuxer_opts *opts);
/** 销毁解复用器及其 TS 解析状态与当前帧缓冲；对 NULL 安全。 */
ZCS_API void zcs_mpegts_demuxer_destroy(zcs_mpegts_demuxer *d);

/** 喂入原始 TS 字节流（188 字节包无需对齐）。 */
ZCS_API zn_err_t zcs_mpegts_demuxer_feed(zcs_mpegts_demuxer *d, const uint8_t *data, size_t len);
/**
 * 冲刷尾部：处理残留分段缓冲、flush 底层 TS 解析器并吐出最后一个 H.264 AU。
 * 流结束或需要立即产出尾帧时调用；flush 后仍可继续 feed。
 */
ZCS_API zn_err_t zcs_mpegts_demuxer_flush(zcs_mpegts_demuxer *d);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_DEMUXER_H */
