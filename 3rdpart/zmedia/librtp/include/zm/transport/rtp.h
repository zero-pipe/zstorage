#ifndef ZM_TRANSPORT_RTP_H
#define ZM_TRANSPORT_RTP_H

/**
 * @file rtp.h
 * @brief RTP 作为 transport-codec 的 reader/writer 接口。
 *
 * reader：输入 RTP/RTCP 包，输出 zm_packet；
 * writer：输入 zm_packet，输出完整 RTP 包到 sink。
 * RTCP 调度、NACK/TWCC 等会话策略留在宿主。
 */
#include "zm/core/reader.h"
#include "zm/core/writer.h"

/* 接口同时导出 RTP/RTCP 包格式与 payload 编解码 helper，使宿主只依赖
 * zm/transport/rtp.h，而无需直接 include librtp 的 C 头。 */
#include "rtp-demuxer.h"
#include "rtp-packet.h"
#include "rtp-payload.h"
#include "rtp-profile.h"
#include "rtp-queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_rtp_reader_cfg {
    zm_codec_id codec;   /**< 优先使用；INVALID 时按 encoding 推导 */
    int payload_type;     /**< >0 时使用；否则按 codec 描述表 */
    const char *encoding; /**< 可为 NULL；按 codec 描述表 */
    int clock_hz;         /**< <=0 时按 codec 描述表 */
    int jitter_ms;        /**< 0 = 不做 jitter/reorder */
} zm_rtp_reader_cfg;

typedef struct zm_rtp_writer_cfg {
    zm_codec_id codec;
    int payload_type;
    const char *encoding;
    int clock_hz;
    uint16_t seq;
    uint32_t ssrc;
    int max_packet_bytes; /**< >0 时设置全局 RTP MTU */
} zm_rtp_writer_cfg;

ZM_API zm_reader *zm_rtp_reader_open(const zm_rtp_reader_cfg *cfg);
ZM_API int zm_rtp_reader_input_rtp(zm_reader *r, const void *pkt, size_t len);

/** 生成 RTCP 报告；返回写入长度，<0 表示无需发送/错误。 */
ZM_API int zm_rtp_reader_rtcp_report(zm_reader *r, void *buf, int len);

ZM_API zm_writer *zm_rtp_writer_open(const zm_sink *sink, const zm_rtp_writer_cfg *cfg);

/** 以显式 RTP 时间戳打包一帧（会话自行管理时间线/seek 重基）。 */
ZM_API int zm_rtp_writer_write_ts(zm_writer *w, const void *data, size_t len, uint32_t rtp_ts);

#ifdef __cplusplus
}
#endif

#endif /* ZM_TRANSPORT_RTP_H */
