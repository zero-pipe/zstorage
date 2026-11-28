/**
 * @file play_rtp_internal.h
 * @brief WebRTC 播放发送面（RTP 队列 / NACK 缓存 / 节拍 / 自适应）与
 *        play_session.c 之间的内部契约。
 *
 * play_rtp.c 负责「一帧 RTP 如何排队、发出去、按 TWCC 降速、丢帧」；
 * play_session.c 负责会话生命周期、STUN/DTLS、泵与输入分派。
 */
#ifndef ZCS_SRC_SESSION_WEBRTC_PLAY_RTP_INTERNAL_H
#define ZCS_SRC_SESSION_WEBRTC_PLAY_RTP_INTERNAL_H

#include "session/webrtc/session_internal.h"
#include "protocol/webrtc/media_gateway.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include <stddef.h>
#include <stdint.h>

/* ── play_rtp.c 提供给 play_session.c 的接口 ───────────────────────────── */

int webrtc_play_rtp_q_ensure(zcs_webrtc_session *s);
void webrtc_play_rtp_q_free(zcs_webrtc_session *s);
size_t webrtc_play_rtp_q_clear(zcs_webrtc_session *s);
int webrtc_play_rtp_q_push(zcs_webrtc_session *s, zcs_rtp_mux_track track, const uint8_t *rtp,
                           size_t len);
unsigned webrtc_play_rtp_q_flush(zcs_webrtc_session *s, unsigned max_send);
int webrtc_play_send_rtp_item(zcs_webrtc_session *s, uint8_t track, const uint8_t *rtp,
                              size_t len, int cache_plain);
int webrtc_play_nack_retransmit(zcs_webrtc_session *s, uint16_t seq);
void webrtc_play_nack_cache_free(zcs_webrtc_session *s);

/* ── play_session.c 提供给 play_rtp.c 的接口 ───────────────────────────── */

void webrtc_play_diag(zcs_webrtc_session *s, const char *tag, const char *detail);
void webrtc_play_gateway_cfg(const zcs_webrtc_session *s, zcs_webrtc_gateway_ingest_cfg *cfg);
void webrtc_play_try_rtcp_boot(zcs_webrtc_session *s);
void webrtc_play_srtp_roundtrip_check(zcs_webrtc_session *s, const uint8_t *plain,
                                      size_t plain_len, size_t prot_len);

#endif /* ZCS_SRC_SESSION_WEBRTC_PLAY_RTP_INTERNAL_H */
