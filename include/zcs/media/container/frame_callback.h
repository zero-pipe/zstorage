/**
 * @file frame_callback.h
 * @brief 容器 → frame 回调。
 */
#ifndef ZCS_MEDIA_CONTAINER_FRAME_CALLBACK_H
#define ZCS_MEDIA_CONTAINER_FRAME_CALLBACK_H

/**
 * 容器解复用输出的帧回调。
 * 注：帧模型仍由 core/frame.h 提供；P5 迁移到 zm_packet 后本头可去掉该依赖。
 */
#include "zcs/core/frame.h"

typedef void (*zcs_payload_frame_cb)(const zcs_frame *frame, void *user);

#endif /* ZCS_MEDIA_CONTAINER_FRAME_CALLBACK_H */
