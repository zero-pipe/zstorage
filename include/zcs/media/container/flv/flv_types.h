/**
 * @file flv_types.h
 * @brief FLV 类型与常量。
 */
#ifndef ZCS_MEDIA_CONTAINER_FLV_FLV_TYPES_H
#define ZCS_MEDIA_CONTAINER_FLV_FLV_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_flv_tag_type {
    ZCS_FLV_TAG_AUDIO = 8,
    ZCS_FLV_TAG_VIDEO = 9,
    ZCS_FLV_TAG_SCRIPT = 18,
} zcs_flv_tag_type;

typedef struct zcs_flv_tag_header {
    uint8_t tag_type;
    uint32_t data_size;
    uint32_t tag_dts_ms;
    uint32_t stream_id;
} zcs_flv_tag_header;

typedef struct zcs_flv_tag_view {
    zcs_flv_tag_header header;
    const uint8_t *body;
    size_t body_size;
} zcs_flv_tag_view;

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_FLV_FLV_TYPES_H */
