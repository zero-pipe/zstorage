#ifndef ZMS_SRC_BRIDGE_ZM_PS_MUXER_H
#define ZMS_SRC_BRIDGE_ZM_PS_MUXER_H

/**
 * @file zm_ps_muxer.h
 * @brief MPEG-PS 打包桥（libmpeg ps_muxer → 同步回调）。
 *
 * GB28181 设备端把 gop_queue 里的 ES 打成 PS，再交给 RTP 打包。
 * 本文件是唯一允许包含裸 mpeg-ps.h 的地方（bridge 边界）。
 */
#include "zms/media/codec/codec_id.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_zm_ps_muxer zms_zm_ps_muxer;

/** 一个完整 PS pack 的输出回调；ps 内存在回调返回后失效。 */
typedef int (*zms_zm_ps_write_fn)(void *user, int stream_id, const uint8_t *ps, size_t len);

typedef struct zms_zm_ps_muxer_opts {
    zms_zm_ps_write_fn write;
    void *user;
} zms_zm_ps_muxer_opts;

/** @return NULL 失败；否则 muxer 句柄。 */
zms_zm_ps_muxer *zms_zm_ps_muxer_create(const zms_zm_ps_muxer_opts *opts);
void zms_zm_ps_muxer_destroy(zms_zm_ps_muxer *m);

/** 添加视频/音频流；返回 >0 的 stream id（失败 <=0）。 */
int zms_zm_ps_muxer_add_video(zms_zm_ps_muxer *m, zms_codec_id codec);
int zms_zm_ps_muxer_add_audio(zms_zm_ps_muxer *m, zms_codec_id codec);

/**
 * 输入一帧视频 ES（Annex-B；参数集可自带）。
 * @param keyframe 非 0 时置 IDR 标志：每个 IDR 前重发 PSM，便于平台从关键帧起播。
 * @param pts_90k/dts_90k 90kHz 时间戳。
 */
int zms_zm_ps_muxer_input_video(zms_zm_ps_muxer *m, int stream_id, const uint8_t *es,
                                 size_t len, int keyframe, int64_t pts_90k, int64_t dts_90k);

/** 输入一帧音频 ES（G711/AAC 裸帧，不带 ADTS）。 */
int zms_zm_ps_muxer_input_audio(zms_zm_ps_muxer *m, int stream_id, const uint8_t *es,
                                 size_t len, int64_t pts_90k);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_BRIDGE_ZM_PS_MUXER_H */
