/**
 * @file frame_codec_policy.h
 * @brief 帧级 codec 策略注册。
 */
#ifndef ZCS_MEDIA_CODEC_FRAME_POLICY_FRAME_CODEC_POLICY_H
#define ZCS_MEDIA_CODEC_FRAME_POLICY_FRAME_CODEC_POLICY_H

#include "zcs/media/codec/codec_id.h"
#include "zcs/core/frame.h"
#include "zcs/zcs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_frame_codec_ops {
    zcs_codec_id id;
    const char *name;
    int (*is_config_frame)(const zcs_frame *f);
    int (*is_keyframe)(const zcs_frame *f);
    int (*gop_queue_storable)(const zcs_frame *f, int cache_started, int has_video);
    int (*gop_queue_new_gop)(const zcs_frame *f, int video_key_pos, int has_video);
} zcs_frame_codec_ops;

/**
 * 按 ops->id 注册（或覆盖）帧级 codec 策略。表容量 32，只存指针，ops 需具备静态生命周期；
 * id 越界或 NULL 时忽略。
 */
ZCS_API void zcs_frame_codec_register(const zcs_frame_codec_ops *ops);
/** 按 codec id 取策略；未注册或 id 越界时返回内置默认策略（永不为 NULL）。 */
ZCS_API const zcs_frame_codec_ops *zcs_frame_codec_find(zcs_codec_id id);
/** 注册全部内建 codec 策略（H.264/H.265/AAC/G.711/AV1/Opus/VP8/VP9/H.266）；幂等。 */
ZCS_API void zcs_frame_codec_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CODEC_FRAME_POLICY_FRAME_CODEC_POLICY_H */
