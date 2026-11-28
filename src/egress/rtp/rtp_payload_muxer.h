/**
 * @file rtp_payload_muxer.h
 * @brief RTP payload 打包器（zm_rtp_writer 之上）。
 */
#ifndef ZCS_SRC_EGRESS_RTP_RTP_PAYLOAD_MUXER_H
#define ZCS_SRC_EGRESS_RTP_RTP_PAYLOAD_MUXER_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct rtp_payload_muxer_t;

/// @param[in] flags 0x0100-packet lost, 0x0200-packet corrupt
/// @return 0-ok, other-error
typedef int (*rtp_payload_muxer_onpacket)(void* param, int pid, const void* data, int bytes, uint32_t timestamp, int flags);

struct rtp_payload_muxer_t* rtp_payload_muxer_create(rtp_payload_muxer_onpacket onpacket, void* param);
int rtp_payload_muxer_destroy(struct rtp_payload_muxer_t* muxer);

/// @param[in] proto RTP/AVP（仅保留兼容签名，内部走 zm_rtp_writer）
/// @param[in] payload rtp payload id, e.g. 96
/// @param[in] encoding rtp payload encoding(for payload > 96 only)
/// @return >=0-paylad index, <0-error
int rtp_payload_muxer_add_payload(struct rtp_payload_muxer_t* muxer, const char* proto, int frequence, int payload, const char* encoding, uint16_t seq, uint32_t ssrc, uint16_t port, const void* extra, int size);

/// @param[in] pid payload index, create by rtp_payload_muxer_add_payload
/// @param[in] codec rtp media codec id
/// @return >=0-media index (for rtp_payload_muxer_input), <0-error
int rtp_payload_muxer_add_media(struct rtp_payload_muxer_t* muxer, int pid, int codec, const void* extra, int size);

/// Get the RTP seq/timestamp the next packet will use
/// @return 0-ok, <0-error
int rtp_payload_muxer_getinfo(struct rtp_payload_muxer_t* muxer, int pid, uint16_t* seq, uint32_t* timestamp, const char** sdp, int *size);

/// @param[in] mid media index, create by rtp_payload_muxer_add_media
/// @return 0-ok, <0-error
int rtp_payload_muxer_input(struct rtp_payload_muxer_t* muxer, int mid, int64_t pts, int64_t dts, const void* data, int bytes, int flags);

/** Seed payload RTP timeline before rtp_payload_muxer_input (ZCS play_clock absolute ts). */
int rtp_payload_muxer_sync_timeline(struct rtp_payload_muxer_t* muxer, int pid, int64_t dts_ms, uint32_t rtp_ts);

#if defined(__cplusplus)
}
#endif
#endif /* !ZCS_SRC_EGRESS_RTP_RTP_PAYLOAD_MUXER_H */
