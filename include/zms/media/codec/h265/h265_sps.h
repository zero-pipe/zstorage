/**
 * @file h265_sps.h
 * @brief H.265 SPS 解析（分辨率）。
 */
#ifndef ZMS_MEDIA_CODEC_H265_H265_SPS_H
#define ZMS_MEDIA_CODEC_H265_H265_SPS_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_h265_sps_info {
    int width;
    int height;
} zms_h265_sps_info;

/** 解析 H265 SPS NAL（含 2 字节 NAL 头）。成功返回 1。 */
ZMS_API int zms_h265_sps_parse(const uint8_t *sps, size_t len, zms_h265_sps_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_H265_H265_SPS_H */
