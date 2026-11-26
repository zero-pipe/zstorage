/**
 * @file streamid.h
 * @brief SRT streamid 解析。
 */
#ifndef ZMS_PROTOCOL_SRT_STREAMID_H
#define ZMS_PROTOCOL_SRT_STREAMID_H

#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_srt_stream_mode {
    ZMS_SRT_MODE_PUBLISH = 0,
    ZMS_SRT_MODE_PLAY = 1,
    ZMS_SRT_MODE_INVALID = 2,
} zms_srt_stream_mode;

/** 解析 ZLM 风格 streamid（如 #!::r=live/test,m=publish）。 */
ZMS_API int zms_srt_streamid_parse(const char *streamid, char *app, char *stream,
                                   zms_srt_stream_mode *mode);
/** 提取 streamid 中的 vhost= 字段；无则返回 0。 */
ZMS_API int zms_srt_streamid_vhost(const char *streamid, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_SRT_STREAMID_H */
