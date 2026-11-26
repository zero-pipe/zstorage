/**
 * @file mkv_file_buffer.h
 * @brief MKV 读取的文件缓冲适配（FILE* → mkv_buffer_t）。
 */
#ifndef ZMS_SRC_MEDIA_CONTAINER_MKV_FILE_BUFFER_H
#define ZMS_SRC_MEDIA_CONTAINER_MKV_FILE_BUFFER_H

#include "zm/format/mkv.h"

#ifdef __cplusplus
extern "C" {
#endif

const struct mkv_buffer_t *zms_mkv_file_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_MEDIA_CONTAINER_MKV_FILE_BUFFER_H */
