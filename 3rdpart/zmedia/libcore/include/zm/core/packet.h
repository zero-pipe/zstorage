#ifndef ZM_CORE_PACKET_H
#define ZM_CORE_PACKET_H

/**
 * @file packet.h
 * @brief 唯一的 ES 边界对象，取代 ZMS 的 zms_frame / zms_gop_slot。
 */
#include "zm/core/mem.h"
#include "zm/core/types.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_packet {
    zm_mem mem;          /**< 载荷；生命周期见 zm_mem */
    int64_t pts_ms;       /**< 显示时间戳（ms），可与 dts_ms 相同 */
    int64_t dts_ms;       /**< 解码时间戳（ms） */
    uint32_t track_id;    /**< reader/writer 实例内有效 */
    zm_codec_id codec;
    zm_kind kind;
    uint16_t flags;       /**< zm_pkt_flag */
    uint8_t es_format;    /**< zm_es_format */
    uint8_t reserved;
} zm_packet;

/** 原始转发单元（仅 relay 使用，不塞进 packet union）。 */
typedef enum zm_raw_kind {
    ZM_RAW_FLV_TAG_BODY = 1,
    ZM_RAW_FLV_TAG = 2,
    ZM_RAW_RTP = 3,
} zm_raw_kind;

typedef struct zm_raw_unit {
    zm_raw_kind kind;
    uint8_t type;      /**< FLV type_id (8/9/18)，其他格式为 0 */
    uint32_t dts_ms;
    zm_mem body;
} zm_raw_unit;

ZM_API void zm_packet_init(zm_packet *p);
ZM_API void zm_packet_clear(zm_packet *p);
ZM_API void zm_raw_unit_init(zm_raw_unit *u);
ZM_API void zm_raw_unit_clear(zm_raw_unit *u);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_PACKET_H */
