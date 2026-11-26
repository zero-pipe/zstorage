#ifndef ZM_TRANSPORT_SDP_H
#define ZM_TRANSPORT_SDP_H

/**
 * @file sdp.h
 * @brief SDP -> 归一化轨道描述。
 *
 * 只做媒体描述解析；RTSP 方法/会话状态机留在 ZMS。
 */
#include "zm/core/track.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_sdp_track {
    zm_kind kind;
    zm_codec_id codec;
    int payload_type;
    int clock_hz;
    int channels;
    char encoding[32];
    char fmtp[512];
    char control[128];
} zm_sdp_track;

typedef struct zm_sdp zm_sdp;

ZM_API zm_sdp *zm_sdp_parse(const char *text, size_t len);
ZM_API size_t zm_sdp_track_count(const zm_sdp *s);
ZM_API const zm_sdp_track *zm_sdp_track_at(const zm_sdp *s, size_t index);
ZM_API void zm_sdp_destroy(zm_sdp *s);

#ifdef __cplusplus
}
#endif

#endif /* ZM_TRANSPORT_SDP_H */
