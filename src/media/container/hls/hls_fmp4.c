/**
 * @file hls_fmp4.c
 * @brief fMP4 分片 muxer 接口实现（封装 zm libhls hls_fmp4）。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/container/hls/hls_fmp4.h"
#include "zm/format/hls.h"
#include "zm/format/mov.h"

static uint8_t mov_object_of_codec(zcs_codec_id codec)
{
    switch (codec) {
    case ZCS_CODEC_H264:
        return MOV_OBJECT_H264;
    case ZCS_CODEC_H265:
        return MOV_OBJECT_H265;
    case ZCS_CODEC_AV1:
        return MOV_OBJECT_AV1;
    case ZCS_CODEC_VP9:
        return MOV_OBJECT_VP9;
    case ZCS_CODEC_VP8:
        return MOV_OBJECT_VP8;
    case ZCS_CODEC_AAC:
        return MOV_OBJECT_AAC;
    case ZCS_CODEC_OPUS:
        return MOV_OBJECT_OPUS;
    default:
        return 0;
    }
}

static int mov_flags_of(int flags)
{
    int out = 0;

    if (flags & ZCS_HLS_FMP4_FLAG_KEYFRAME) {
        out |= MOV_AV_FLAG_KEYFREAME;
    }
    if (flags & ZCS_HLS_FMP4_FLAG_SEGMENT_DISABLE) {
        out |= MOV_AV_FLAG_SEGMENT_DISABLE;
    }
    return out;
}

zcs_hls_fmp4 *zcs_hls_fmp4_create(int64_t segment_ms, zcs_hls_fmp4_segment_cb cb, void *param)
{
    return (zcs_hls_fmp4 *)hls_fmp4_create(segment_ms, (hls_fmp4_handler)cb, param);
}

void zcs_hls_fmp4_destroy(zcs_hls_fmp4 *m)
{
    hls_fmp4_destroy((hls_fmp4_t *)m);
}

int zcs_hls_fmp4_add_video(zcs_hls_fmp4 *m, zcs_codec_id codec, int width, int height,
                           const void *extra, size_t extra_len)
{
    return hls_fmp4_add_video((hls_fmp4_t *)m, mov_object_of_codec(codec), width, height, extra,
                              extra_len);
}

int zcs_hls_fmp4_add_audio(zcs_hls_fmp4 *m, zcs_codec_id codec, int channels, int bits_per_sample,
                           int sample_rate, const void *extra, size_t extra_len)
{
    return hls_fmp4_add_audio((hls_fmp4_t *)m, mov_object_of_codec(codec), channels,
                              bits_per_sample, sample_rate, extra, extra_len);
}

int zcs_hls_fmp4_write_frame(zcs_hls_fmp4 *m, int track, const void *data, size_t bytes,
                             int64_t pts_ms, int64_t dts_ms, int flags)
{
    return hls_fmp4_input((hls_fmp4_t *)m, track, data, bytes, pts_ms, dts_ms, mov_flags_of(flags));
}

int zcs_hls_fmp4_init_segment(zcs_hls_fmp4 *m, void *out, size_t cap)
{
    return hls_fmp4_init_segment((hls_fmp4_t *)m, out, cap);
}
