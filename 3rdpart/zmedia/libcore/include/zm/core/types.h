#ifndef ZM_CORE_TYPES_H
#define ZM_CORE_TYPES_H

/**
 * @file types.h
 * @brief ZM 共用媒体模型的标量类型：状态码、轨道种类、codec id、帧格式、包标志。
 *
 * 本文件零依赖：不 include znet / zms，也不 include 任何容器头。
 */
#include "zm/zm_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 统一状态码（负数失败）。 */
typedef enum zm_status {
    ZM_OK = 0,
    ZM_ERR_INVALID = -1,
    ZM_ERR_NOMEM = -2,
    ZM_ERR_NOT_SUPPORTED = -3,
    ZM_ERR_AGAIN = -4,
    ZM_ERR_IO = -5,
    ZM_ERR_BUFFER_TOO_SMALL = -6,
} zm_status;

/** 轨道种类。 */
typedef enum zm_kind {
    ZM_KIND_UNKNOWN = 0,
    ZM_KIND_VIDEO = 1,
    ZM_KIND_AUDIO = 2,
    ZM_KIND_SUBTITLE = 3,
    ZM_KIND_DATA = 4,
} zm_kind;

/**
 * 规范 codec id。1..10 与 ZMS 现有 zms_codec_id 数值对齐，迁移期可直接别名。
 * 新增 codec 只在这里追加，不新建 per-codec 适配文件。
 */
typedef enum zm_codec_id {
    ZM_CODEC_INVALID = 0,
    ZM_CODEC_H264 = 1,
    ZM_CODEC_H265 = 2,
    ZM_CODEC_AAC = 3,
    ZM_CODEC_OPUS = 4,
    ZM_CODEC_G711A = 5,
    ZM_CODEC_G711U = 6,
    ZM_CODEC_AV1 = 7,
    ZM_CODEC_VP8 = 8,
    ZM_CODEC_VP9 = 9,
    ZM_CODEC_H266 = 10,
    ZM_CODEC_MP3 = 11,
    ZM_CODEC_AC3 = 12,
    ZM_CODEC_EAC3 = 13,
    ZM_CODEC_AVS3 = 14,
    ZM_CODEC_MPEG4 = 15,
    ZM_CODEC_H263 = 16,
    ZM_CODEC_FLAC = 17,
    ZM_CODEC_MAX
} zm_codec_id;

/** 载荷的 ES 帧格式（边界上声明，writer 按需转换）。 */
typedef enum zm_es_format {
    ZM_ES_ANNEXB = 0,        /**< H.264/H.265/H.266 start-code */
    ZM_ES_RAW = 1,           /**< raw AAC / AV1 OBU / VP8/VP9 / Opus / G.711 */
    ZM_ES_ADTS = 2,          /**< ADTS pass-through */
    ZM_ES_LENGTH_PREFIXED = 3, /**< 4-byte NAL length (MP4/FLV native) */
} zm_es_format;

/** 包标志。 */
typedef enum zm_pkt_flag {
    ZM_PKT_KEY = 0x0001,         /**< decode-start / IDR / sync sample */
    ZM_PKT_CONFIG = 0x0002,      /**< parameter set / ASC / OpusHead */
    ZM_PKT_EOS = 0x0004,
    ZM_PKT_DISCARDABLE = 0x0008, /**< SEI/AUD；不推进 GOP 状态 */
    ZM_PKT_LOST = 0x0010,        /**< 之前有丢包（RTP） */
} zm_pkt_flag;

/** 源时间基；规范包时间戳始终是 ms。 */
typedef struct zm_timebase {
    int32_t num;
    int32_t den;
} zm_timebase;

/** 容器 / 传输格式。 */
typedef enum zm_format {
    ZM_FMT_UNKNOWN = 0,
    ZM_FMT_FLV,
    ZM_FMT_MPEG_TS,
    ZM_FMT_MPEG_PS,
    ZM_FMT_MOV,
    ZM_FMT_FMP4,
    ZM_FMT_MKV,
    ZM_FMT_RTP,
    ZM_FMT_HLS,
    ZM_FMT_DASH,
} zm_format;

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_TYPES_H */
