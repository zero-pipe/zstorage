#ifndef ZMS_SRC_BRIDGE_ZM_RTP_INGEST_H
#define ZMS_SRC_BRIDGE_ZM_RTP_INGEST_H

/**
 * @file zm_rtp_ingest.h
 * @brief 通用 RTP ingest 适配：按 track 维护 zm_rtp_reader，输出 zm_packet。
 *
 * RTSP RECORD、RTSP client、WebRTC WHIP 共用；sink 内同步消费 packet。
 */
#include "zm/core/packet.h"
#include "zms/media/codec/codec_id.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_zm_rtp_ingest zms_zm_rtp_ingest;
typedef int (*zms_zm_rtp_packet_sink)(void *user, const zm_packet *pkt);

zms_zm_rtp_ingest *zms_zm_rtp_ingest_create(int max_tracks, int jitter_ms,
                                               zms_zm_rtp_packet_sink sink, void *user);

/**
 * 设置/更新一个 track 的 RTP reader 参数。
 * @param encoding 可为 NULL，按 codec 描述表推导
 */
int zms_zm_rtp_ingest_track(zms_zm_rtp_ingest *z, int track_idx, zms_codec_id codec,
                             int payload_type, int clock_hz, const char *encoding);

/** 输入一个完整 RTP 包（含 12 字节头）。 */
int zms_zm_rtp_ingest_input(zms_zm_rtp_ingest *z, int track_idx, const void *rtp, size_t len);

void zms_zm_rtp_ingest_destroy(zms_zm_rtp_ingest *z);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_BRIDGE_ZM_RTP_INGEST_H */
