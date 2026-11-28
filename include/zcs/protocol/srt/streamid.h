/**
 * @file streamid.h
 * @brief SRT streamid 解析。
 */
#ifndef ZCS_PROTOCOL_SRT_STREAMID_H
#define ZCS_PROTOCOL_SRT_STREAMID_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_srt_stream_mode {
    ZCS_SRT_MODE_PUBLISH = 0,
    ZCS_SRT_MODE_PLAY = 1,
    ZCS_SRT_MODE_INVALID = 2,
} zcs_srt_stream_mode;

/** 解析 ZLM 风格 streamid（如 #!::r=live/test,m=publish）。 */
ZCS_API int zcs_srt_streamid_parse(const char *streamid, char *app, char *stream,
                                   zcs_srt_stream_mode *mode);
/** 提取 streamid 中的 vhost= 字段；无则返回 0。 */
ZCS_API int zcs_srt_streamid_vhost(const char *streamid, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_SRT_STREAMID_H */
