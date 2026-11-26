/**
 * @file sdp.h
 * @brief RTSP SDP 协商。
 */
#ifndef ZMS_PROTOCOL_RTSP_SDP_H
#define ZMS_PROTOCOL_RTSP_SDP_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include "zms/core/media_track.h"
#include "zms/media/media_limits.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 别名：SDP 轨槽与 engine track-slot 上限共用。 */
#define ZMS_SDP_TRACK_MAX ZMS_TRACK_SLOT_MAX

typedef struct zms_sdp_session {
    zms_media_track tracks[ZMS_SDP_TRACK_MAX];
    unsigned track_count;
    char session_control[ZMS_TRACK_CTRL_MAX];
} zms_sdp_session;

/**
 * 解析 SDP 文本并填充会话轨槽（out 会被覆盖写入）。
 * @return ZN_OK 成功；无有效媒体描述/格式非法返回错误
 */
ZMS_API zn_err_t zms_sdp_parse(const char *sdp, size_t len, zms_sdp_session *out);
/** @return 第 index 条轨道（借用，归属会话）；越界或 s 为 NULL 返回 NULL。 */
ZMS_API const zms_media_track *zms_sdp_track_at(const zms_sdp_session *s, unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_RTSP_SDP_H */
