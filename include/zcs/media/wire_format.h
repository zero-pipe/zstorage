/**
 * @file wire_format.h
 * @brief 协议规范通用定义（FLV/RTP 消息类型等）。
 */
#ifndef ZCS_MEDIA_WIRE_FORMAT_H
#define ZCS_MEDIA_WIRE_FORMAT_H

/**
 * 播放出站或载荷解复用使用的协议规范（与 zcs_codec_id 正交）。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_wire_format_id {
    ZCS_WIRE_FORMAT_INVALID = 0,
    ZCS_WIRE_FORMAT_RTMP,
    ZCS_WIRE_FORMAT_RTP,
    ZCS_WIRE_FORMAT_MPEG_TS,
    ZCS_WIRE_FORMAT_MPEG_PS,
    ZCS_WIRE_FORMAT_MP4,
    ZCS_WIRE_FORMAT_HTTP_FLV,
    ZCS_WIRE_FORMAT_SRT,
} zcs_wire_format_id;

typedef enum zcs_wire_format_dir {
    ZCS_WIRE_FORMAT_DIR_DECODE = 1,
    ZCS_WIRE_FORMAT_DIR_ENCODE = 2,
} zcs_wire_format_dir;

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_WIRE_FORMAT_H */
