/**
 * @file sdp.h
 * @brief RTSP SDP 协商。
 */
#ifndef ZCS_PROTOCOL_RTSP_SDP_H
#define ZCS_PROTOCOL_RTSP_SDP_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zcs/core/media_track.h"
#include "zcs/media/media_limits.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 别名：SDP 轨槽与 engine track-slot 上限共用。 */
#define ZCS_SDP_TRACK_MAX ZCS_TRACK_SLOT_MAX

typedef struct zcs_sdp_session {
    zcs_media_track tracks[ZCS_SDP_TRACK_MAX];
    unsigned track_count;
    char session_control[ZCS_TRACK_CTRL_MAX];
} zcs_sdp_session;

/**
 * 解析 SDP 文本并填充会话轨槽（out 会被覆盖写入）。
 * @return ZN_OK 成功；无有效媒体描述/格式非法返回错误
 */
ZCS_API zn_err_t zcs_sdp_parse(const char *sdp, size_t len, zcs_sdp_session *out);
/** @return 第 index 条轨道（借用，归属会话）；越界或 s 为 NULL 返回 NULL。 */
ZCS_API const zcs_media_track *zcs_sdp_track_at(const zcs_sdp_session *s, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_SDP_H */
