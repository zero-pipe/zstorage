/**
 * @file frame_callback.h
 * @brief 容器 → frame 回调。
 */
#ifndef ZMS_MEDIA_CONTAINER_FRAME_CALLBACK_H
#define ZMS_MEDIA_CONTAINER_FRAME_CALLBACK_H

/**
 * 容器解复用输出的帧回调。
 * 注：帧模型仍由 core/frame.h 提供；P5 迁移到 zm_packet 后本头可去掉该依赖。
 */
#include "zms/core/frame.h"

typedef void (*zms_payload_frame_cb)(const zms_frame *frame, void *user);

#endif /* ZMS_MEDIA_CONTAINER_FRAME_CALLBACK_H */
