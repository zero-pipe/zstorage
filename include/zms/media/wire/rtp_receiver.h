/**
 * @file rtp_receiver.h
 * @brief RTP 接收（jitter/reorder 队列）。
 */
#ifndef ZMS_MEDIA_WIRE_RTP_RECEIVER_H
#define ZMS_MEDIA_WIRE_RTP_RECEIVER_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include "zms/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_rtp_sorted_cb)(const zms_rtp_packet *pkt, int track_index, void *user);

typedef struct zms_rtp_receiver zms_rtp_receiver;

typedef struct zms_rtp_receiver_opts {
    unsigned max_track;
    unsigned jitter_slots;
    zms_rtp_sorted_cb on_sorted;
    void *user;
    /** 跳过 seq 缺口前的重排保持（毫秒）；0 → 200（ZLM 风格 rtp sort）。 */
    int jitter_ms;
} zms_rtp_receiver_opts;

/**
 * 创建 RTP 接收器：为每个 track 建一条重排队列（jitter buffer）。
 * max_track 必须 >0；jitter_ms<=0 时用默认值。
 * @return 接收器；分配失败返回 NULL（用 destroy 释放）。
 */
ZMS_API zms_rtp_receiver *zms_rtp_receiver_create(const zms_rtp_receiver_opts *opts);
/** 销毁接收器与全部重排队列；r 可为 NULL。 */
ZMS_API void zms_rtp_receiver_destroy(zms_rtp_receiver *r);
/**
 * 送入一包 RTP（原始字节），进入重排队列后按序回调 on_sorted。
 * @param track_index 轨道序号，须在 [0,max_track) 内
 * @return ZN_OK 成功；队列满返回 ZN_ERR_AGAIN；参数/解析非法返回错误。
 */
ZMS_API zn_err_t zms_rtp_receiver_input(zms_rtp_receiver *r, int track_index, const uint8_t *data,
                                         size_t len);
/** 清空并重建所有重排队列（丢弃在途包）；用于换源/重连。 */
ZMS_API void zms_rtp_receiver_flush(zms_rtp_receiver *r);
/** 复位接收器（等价于 flush）。 */
ZMS_API void zms_rtp_receiver_reset(zms_rtp_receiver *r);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_RTP_RECEIVER_H */
