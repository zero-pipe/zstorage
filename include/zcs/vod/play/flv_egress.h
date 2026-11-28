/**
 * @file flv_egress.h
 * @brief VOD FLV 出站。
 */
#ifndef ZCS_VOD_PLAY_FLV_EGRESS_H
#define ZCS_VOD_PLAY_FLV_EGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_vod_buffer_reader;
struct zcs_egress_clock;

/** egress_pipeline 与 vod/flv_muxer 共用的 VOD fifo → FLV tag 出站绑定。 */
typedef struct zcs_flv_vod_egress_bind {
    struct zcs_vod_buffer_reader *vod_rd;
    struct zcs_egress_clock *play_clk;
    int *catchup_left;
    uint8_t **es_buf;
    size_t *es_cap;
    /** 1 = epoch 锁定后按 play_clock pacing（HTTP-FLV 实时 / RTMP VOD）。 */
    int pace_when_locked;
} zcs_flv_vod_egress_bind;

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_PLAY_FLV_EGRESS_H */
