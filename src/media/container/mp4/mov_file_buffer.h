/**
 * @file mov_file_buffer.h
 * @brief MOV/MP4 文件读取缓冲。
 */
#ifndef ZMS_SRC_MEDIA_CONTAINER_MP4_MOV_FILE_BUFFER_H
#define ZMS_SRC_MEDIA_CONTAINER_MP4_MOV_FILE_BUFFER_H

#include "zm/format/mov.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const struct mov_buffer_t *zms_mov_file_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_MEDIA_CONTAINER_MP4_MOV_FILE_BUFFER_H */
