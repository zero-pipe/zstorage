/**
 * @file codec_filter.h
 * @brief 会话能力与角色过滤。
 */
#ifndef ZCS_CORE_CODEC_FILTER_H
#define ZCS_CORE_CODEC_FILTER_H

#include "zcs/media/codec/codec_id.h"
#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_session_cap_role {
    ZCS_PROTO_CAP_RTSP_PLAY = 0,
    ZCS_PROTO_CAP_RTSP_PUBLISH,
    ZCS_PROTO_CAP_RTMP_PLAY,
    ZCS_PROTO_CAP_RTMP_PUBLISH,
    ZCS_PROTO_CAP_HTTP_FLV_PLAY,
    ZCS_PROTO_CAP_HTTP_TS_PLAY,
    ZCS_PROTO_CAP_WEBRTC_PLAY,
    ZCS_PROTO_CAP_WEBRTC_PUBLISH,
    ZCS_PROTO_CAP_SRT_PLAY,
    ZCS_PROTO_CAP_SRT_PUBLISH,
    ZCS_PROTO_CAP_RTP_PS_PUBLISH,
    ZCS_PROTO_CAP_HLS_PLAY,
    ZCS_PROTO_CAP_DASH_PLAY,
} zcs_session_cap_role;

/** 按音视频 codec 检查能力；无轨时传 ZCS_CODEC_INVALID*/
ZCS_API zn_err_t zcs_session_capability_check(zcs_session_cap_role role, zcs_codec_id video,
                                               zcs_codec_id audio, int has_video, int has_audio);

/** source 轨道/config 读取 codec 后检查 */
ZCS_API zn_err_t zcs_session_capability_check_source(zcs_session_cap_role role,
                                                      const zcs_media_source *src);

/** 推流入站：按 source schema / publish_origin 选择 publish 能力位 */
ZCS_API zn_err_t zcs_session_capability_check_publish_source(const zcs_media_source *src);

/** 由播放器标识字符串映射到播放能力角色；空/未知一律按 RTMP 播放处理。 */
ZCS_API zcs_session_cap_role zcs_session_capability_play_role(const char *player);
/** 由源的 publish_origin/schema 推导推流能力角色；src 为 NULL 时按 RTMP 推流处理。 */
ZCS_API zcs_session_cap_role zcs_session_capability_publish_role(const zcs_media_source *src);

/** 角色的稳定字符串名（如 "rtsp-play"），用于日志；未知返回 "unknown"。 */
ZCS_API const char *zcs_session_capability_role_name(zcs_session_cap_role role);
/** 该角色允许的 codec 组合说明（用于拒绝时提示）；未知角色返回空串。 */
ZCS_API const char *zcs_session_capability_allowed_hint(zcs_session_cap_role role);

/** 以 warn 打印一次能力拒绝日志（player/源/允许集合），便于定位不兼容 codec。 */
ZCS_API void zcs_session_capability_log_reject(const char *player, const zcs_media_source *src,
                                               zcs_session_cap_role role);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_CODEC_FILTER_H */
