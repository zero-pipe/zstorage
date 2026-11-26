/**
 * @file wire_format.h
 * @brief 协议规范通用定义（FLV/RTP 消息类型等）。
 */
#ifndef ZMS_MEDIA_WIRE_FORMAT_H
#define ZMS_MEDIA_WIRE_FORMAT_H

/**
 * 播放出站或载荷解复用使用的协议规范（与 zms_codec_id 正交）。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_wire_format_id {
    ZMS_WIRE_FORMAT_INVALID = 0,
    ZMS_WIRE_FORMAT_RTMP,
    ZMS_WIRE_FORMAT_RTP,
    ZMS_WIRE_FORMAT_MPEG_TS,
    ZMS_WIRE_FORMAT_MPEG_PS,
    ZMS_WIRE_FORMAT_MP4,
    ZMS_WIRE_FORMAT_HTTP_FLV,
    ZMS_WIRE_FORMAT_SRT,
} zms_wire_format_id;

typedef enum zms_wire_format_dir {
    ZMS_WIRE_FORMAT_DIR_DECODE = 1,
    ZMS_WIRE_FORMAT_DIR_ENCODE = 2,
} zms_wire_format_dir;

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_FORMAT_H */
