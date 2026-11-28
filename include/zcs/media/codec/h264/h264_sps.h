/**
 * @file h264_sps.h
 * @brief H.264 SPS 解析。
 */
#ifndef ZCS_MEDIA_CODEC_H264_H264_SPS_H
#define ZCS_MEDIA_CODEC_H264_H264_SPS_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_h264_sps_info {
    int width;
    int height;
    float fps;
} zcs_h264_sps_info;

/** 解析 H264 SPS NAL（含 0x67 头字节）。成功返回 1。 */
ZCS_API int zcs_h264_sps_parse(const uint8_t *sps, size_t len, zcs_h264_sps_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H264_H264_SPS_H */
