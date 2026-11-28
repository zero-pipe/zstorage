/**
 * @file media_track.h
 * @brief 归一化媒体轨描述。
 */
#ifndef ZCS_CORE_MEDIA_TRACK_H
#define ZCS_CORE_MEDIA_TRACK_H

#include "zcs/core/frame.h"
#include "zcs/zcs_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_TRACK_CTRL_MAX 128
#define ZCS_TRACK_FMTP_MAX 256

/** 已解析的 SDP / RTSP 媒体轨（客户端 RECORD / 播放）。 */
typedef struct zcs_media_track {
    zcs_track_type type;
    zcs_codec_id codec;
    int payload_type;
    int sample_rate;
    int channels;
    int ready;
    uint8_t interleaved_rtp;
    uint8_t interleaved_rtcp;
    char control[ZCS_TRACK_CTRL_MAX];
    char fmtp[ZCS_TRACK_FMTP_MAX];
} zcs_media_track;

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_MEDIA_TRACK_H */
