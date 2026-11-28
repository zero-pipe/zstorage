/**
 * @file h265_sps.h
 * @brief H.265 SPS 解析（分辨率）。
 */
#ifndef ZCS_MEDIA_CODEC_H265_H265_SPS_H
#define ZCS_MEDIA_CODEC_H265_H265_SPS_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_h265_sps_info {
    int width;
    int height;
} zcs_h265_sps_info;

/** 解析 H265 SPS NAL（含 2 字节 NAL 头）。成功返回 1。 */
ZCS_API int zcs_h265_sps_parse(const uint8_t *sps, size_t len, zcs_h265_sps_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_H265_H265_SPS_H */
