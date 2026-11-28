/**
 * @file rtp_receiver.h
 * @brief RTP 接收（jitter/reorder 队列）。
 */
#ifndef ZCS_MEDIA_WIRE_RTP_RECEIVER_H
#define ZCS_MEDIA_WIRE_RTP_RECEIVER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zcs/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zcs_rtp_sorted_cb)(const zcs_rtp_packet *pkt, int track_index, void *user);

typedef struct zcs_rtp_receiver zcs_rtp_receiver;

typedef struct zcs_rtp_receiver_opts {
    unsigned max_track;
    unsigned jitter_slots;
    zcs_rtp_sorted_cb on_sorted;
    void *user;
    /** 跳过 seq 缺口前的重排保持（毫秒）；0 → 200（ZLM 风格 rtp sort）。 */
    int jitter_ms;
} zcs_rtp_receiver_opts;

/**
 * 创建 RTP 接收器：为每个 track 建一条重排队列（jitter buffer）。
 * max_track 必须 >0；jitter_ms<=0 时用默认值。
 * @return 接收器；分配失败返回 NULL（用 destroy 释放）。
 */
ZCS_API zcs_rtp_receiver *zcs_rtp_receiver_create(const zcs_rtp_receiver_opts *opts);
/** 销毁接收器与全部重排队列；r 可为 NULL。 */
ZCS_API void zcs_rtp_receiver_destroy(zcs_rtp_receiver *r);
/**
 * 送入一包 RTP（原始字节），进入重排队列后按序回调 on_sorted。
 * @param track_index 轨道序号，须在 [0,max_track) 内
 * @return ZN_OK 成功；队列满返回 ZN_ERR_AGAIN；参数/解析非法返回错误。
 */
ZCS_API zn_err_t zcs_rtp_receiver_input(zcs_rtp_receiver *r, int track_index, const uint8_t *data,
                                         size_t len);
/** 清空并重建所有重排队列（丢弃在途包）；用于换源/重连。 */
ZCS_API void zcs_rtp_receiver_flush(zcs_rtp_receiver *r);
/** 复位接收器（等价于 flush）。 */
ZCS_API void zcs_rtp_receiver_reset(zcs_rtp_receiver *r);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_WIRE_RTP_RECEIVER_H */
