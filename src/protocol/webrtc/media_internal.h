/**
 * @file media_internal.h
 * @brief WebRTC 媒体内部辅助。
 */
#ifndef ZMS_SRC_PROTOCOL_WEBRTC_MEDIA_INTERNAL_H
#define ZMS_SRC_PROTOCOL_WEBRTC_MEDIA_INTERNAL_H

#include "zms/media/wire/srtp.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int zms_webrtc_stun_is_binding_req(const uint8_t *data, size_t len);
size_t zms_webrtc_stun_binding_reply(const uint8_t *req, size_t req_len, uint8_t *out,
                                     size_t out_cap, uint32_t xor_ip_be, uint16_t xor_port,
                                     const char *pwd);

int zms_webrtc_dtls_global_init(void);
void zms_webrtc_dtls_global_fini(void);
const char *zms_webrtc_dtls_fingerprint(void);

typedef struct zms_webrtc_dtls zms_webrtc_dtls;

zms_webrtc_dtls *zms_webrtc_dtls_create(void);
zms_webrtc_dtls *zms_webrtc_dtls_create_client(void);
void zms_webrtc_dtls_destroy(zms_webrtc_dtls *d);
/** 仅 Client 角色：握手未起时发出 DTLS ClientHello。 */
int zms_webrtc_dtls_kick(zms_webrtc_dtls *d, uint8_t *out, size_t out_cap, size_t *out_len);
/** @return 1 已连接，0 进行中，-1 错误。有待发响应时写入 out。 */
int zms_webrtc_dtls_input(zms_webrtc_dtls *d, const uint8_t *pkt, size_t len, uint8_t *out,
                          size_t out_cap, size_t *out_len);
int zms_webrtc_dtls_is_connected(const zms_webrtc_dtls *d);
int zms_webrtc_dtls_export_server_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt);
int zms_webrtc_dtls_export_client_srtp(const zms_webrtc_dtls *d, uint8_t *key, uint8_t *salt);

int zms_webrtc_packet_is_dtls(const uint8_t *data, size_t len);
int zms_webrtc_packet_is_rtp(const uint8_t *data, size_t len);

/** 薄封装 zms/session/rtp/rtcp.h，供 WebRTC 复合包解析。 */
int zms_webrtc_rtcp_is_psfb_pli(const uint8_t *data, size_t len);
int zms_webrtc_rtcp_is_rtpfb_nack(const uint8_t *data, size_t len);
int zms_webrtc_rtcp_is_rtpfb_twcc(const uint8_t *data, size_t len);
/** Transport-CC 反馈统计（RFC 8888 RTPFB FMT=15）。 */
typedef struct zms_webrtc_twcc_summary {
    uint16_t base_seq;
    uint16_t pkt_count;
    uint32_t received;
    uint32_t lost;
    uint32_t recovered;
    uint32_t parsed;
} zms_webrtc_twcc_summary;
/** 解析 TWCC 反馈首块；成功返回 0。 */
int zms_webrtc_rtcp_parse_twcc(const uint8_t *data, size_t len, zms_webrtc_twcc_summary *out);
/** 解析 RR 第一个 report block（fraction lost / jitter / LSR / DLSR）。 */
int zms_webrtc_rtcp_parse_rr(const uint8_t *data, size_t len, uint32_t *fraction_lost,
                             uint32_t *jitter, uint32_t *lsr, uint32_t *dlsr);
size_t zms_webrtc_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                uint32_t octet_count);
void zms_webrtc_rtcp_for_each_compound(const uint8_t *data, size_t len,
                                       void (*cb)(const uint8_t *block, size_t block_len,
                                                  void *user),
                                       void *user);

size_t zms_webrtc_rtp_add_twcc_ext(uint8_t *rtp, size_t len, size_t cap, int ext_id,
                                   uint16_t twcc_seq, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_PROTOCOL_WEBRTC_MEDIA_INTERNAL_H */
