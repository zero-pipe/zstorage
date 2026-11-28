#ifndef ZCS_MEDIA_CONTAINER_HLS_HLS_FMP4_H
#define ZCS_MEDIA_CONTAINER_HLS_HLS_FMP4_H

/**
 * @file hls_fmp4.h
 * @brief fMP4（CMAF）分片 muxer 接口（基于 zm libhls hls_fmp4 API）。
 *
 * 使播放/业务层无需包含 `hls-fmp4.h` 与 `mov-format.h`：按 zcs_codec_id 添加轨，
 * 以 ZCS flag 位喂样本，接口映射到底层 MOV_OBJECT_* / MOV_AV_FLAG_*。
 */
#include "zcs/zcs_export.h"
#include "zcs/media/codec/codec_id.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_hls_fmp4 zcs_hls_fmp4;

/** 分片数据 sink（一条 init/媒体 fragment）。@return 0 成功。 */
typedef int (*zcs_hls_fmp4_segment_cb)(void *param, const void *data, size_t bytes, int64_t pts_ms,
                                       int64_t dts_ms, int64_t duration_ms);

/** zcs_hls_fmp4_write_frame() 的样本标志。 */
#define ZCS_HLS_FMP4_FLAG_KEYFRAME 0x01
#define ZCS_HLS_FMP4_FLAG_SEGMENT_DISABLE 0x02

/** @param segment_ms 分片时长（毫秒）；0 → 每个视频关键帧一片。 */
ZCS_API zcs_hls_fmp4 *zcs_hls_fmp4_create(int64_t segment_ms, zcs_hls_fmp4_segment_cb cb,
                                          void *param);
/** 销毁 fMP4 muxer，释放底层 hls_fmp4（不回调 sink）；m 可为 NULL。 */
ZCS_API void zcs_hls_fmp4_destroy(zcs_hls_fmp4 *m);

/** 为 @p codec 添加视频轨。@return 轨 id（>=0），<0 错误。 */
ZCS_API int zcs_hls_fmp4_add_video(zcs_hls_fmp4 *m, zcs_codec_id codec, int width, int height,
                                   const void *extra, size_t extra_len);

/** 为 @p codec 添加音频轨。@return 轨 id（>=0），<0 错误。 */
ZCS_API int zcs_hls_fmp4_add_audio(zcs_hls_fmp4 *m, zcs_codec_id codec, int channels,
                                   int bits_per_sample, int sample_rate, const void *extra,
                                   size_t extra_len);

/** 写入一帧样本（H.264/H.265 需 length-prefixed）。@param flags ZCS_HLS_FMP4_FLAG_*。@return 0 成功。*/
ZCS_API int zcs_hls_fmp4_write_frame(zcs_hls_fmp4 *m, int track, const void *data, size_t bytes,
                                     int64_t pts_ms, int64_t dts_ms, int flags);

/** 序列化 init 分片。@return 写入字节数（>0），<0 错误。 */
ZCS_API int zcs_hls_fmp4_init_segment(zcs_hls_fmp4 *m, void *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_HLS_HLS_FMP4_H */
