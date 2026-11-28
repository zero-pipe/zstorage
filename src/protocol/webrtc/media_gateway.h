/**
 * @file media_gateway.h
 * @brief WebRTC 媒体网关接口。
 */
#ifndef ZCS_SRC_PROTOCOL_WEBRTC_MEDIA_GATEWAY_H
#define ZCS_SRC_PROTOCOL_WEBRTC_MEDIA_GATEWAY_H

/**
 * WebRTC UDP 分类 + SDP PT 映射（协议壳，不含 SRTP）
 *
 * SRTP/SRTCP: zcs/webrtc/webrtc_srtp.h
 * Plain RTP:  zcs/media/wire/rtp_packet.h
 * Plain RTCP: zcs/session/rtp/rtcp.h + webrtc_rtcp.c 薄封装
 */
#include "zcs/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** RTSP/FLV mux 共用的 canonical PT（ZM RTP 默认 96/97） */
#define ZCS_WEBRTC_CANON_VIDEO_PT 96u
#define ZCS_WEBRTC_CANON_AUDIO_PT 97u

typedef enum {
    ZCS_WEBRTC_GATEWAY_STUN = 0,
    ZCS_WEBRTC_GATEWAY_DTLS,
    ZCS_WEBRTC_GATEWAY_SRTCP,
    ZCS_WEBRTC_GATEWAY_SRTP,
    ZCS_WEBRTC_GATEWAY_UNKNOWN
} zcs_webrtc_gateway_pkt_kind;

typedef struct zcs_webrtc_gateway_ingest_cfg {
    uint8_t canon_video_pt;
    uint8_t canon_audio_pt;
    /** SDP 协商得到的 wire PT；表示映射到 canonical 的入站 PT */
    uint8_t wire_video_pt;
    uint8_t wire_audio_pt;
    int answer_has_video;
    int answer_has_audio;
} zcs_webrtc_gateway_ingest_cfg;

zcs_webrtc_gateway_pkt_kind zcs_webrtc_gateway_classify(const uint8_t *data, size_t len);

void zcs_webrtc_gateway_ingest_cfg_defaults(zcs_webrtc_gateway_ingest_cfg *cfg);

/** @return 1=视频，0=音频，-1=不匹配 */
int zcs_webrtc_gateway_match_media_track(const zcs_webrtc_gateway_ingest_cfg *cfg, uint8_t pt);

/** 将 SDP 协商 PT 映射为 canonical PT，供 payload demux 与 RTSP 路径一致 */
void zcs_webrtc_gateway_remap_pt(uint8_t *rtp, size_t len, const zcs_webrtc_gateway_ingest_cfg *cfg,
                                 int is_video);

/** WHEP 出站：canonical PT → SDP wire PT（WHIP remap_pt 的逆操作） */
void zcs_webrtc_gateway_remap_pt_egress(uint8_t *rtp, size_t len,
                                        const zcs_webrtc_gateway_ingest_cfg *cfg, int is_video);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_PROTOCOL_WEBRTC_MEDIA_GATEWAY_H */
