/**
 * @file ice_internal.h
 * @brief WebRTC ICE 私有结构（协议层；只依赖 ice_host.h，不依赖会话）。
 */
#ifndef ZCS_SRC_PROTOCOL_WEBRTC_ICE_INTERNAL_H
#define ZCS_SRC_PROTOCOL_WEBRTC_ICE_INTERNAL_H
#include "protocol/webrtc/ice_host.h"
#include <stddef.h>

struct zcs_webrtc_ice;
void zcs_webrtc_ice_port_init(zn_poller *poller);
void zcs_webrtc_ice_port_fini(void);
/** zice stun_timer_start：将 timer param 映射到会话 poller（回退：service poller）。 */
zn_poller *zcs_webrtc_ice_timer_poller(void *param);
struct zcs_webrtc_ice *zcs_webrtc_ice_create(zcs_webrtc_ice_host *host);
void zcs_webrtc_ice_destroy(struct zcs_webrtc_ice *ice);
int zcs_webrtc_ice_setup(struct zcs_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port);
/** 主动模式：设置本地凭据/候选并开始 gather，供生成 offer 使用。 */
int zcs_webrtc_ice_prepare_client(struct zcs_webrtc_ice *ice, const char *local_ufrag,
                                  const char *local_pwd, const char *advertise_host,
                                  uint16_t local_port);
/** 主动模式：写入 answer 的远端凭据/候选并启动连通性检查。 */
int zcs_webrtc_ice_connect_client(struct zcs_webrtc_ice *ice, const char *answer, size_t answer_len,
                                  const char *remote_ufrag, const char *remote_pwd);
/** 追加远端候选（WHEP/WHIP PATCH 的 trickle ICE 片段）。 */
int zcs_webrtc_ice_add_remote_sdp(struct zcs_webrtc_ice *ice, const char *sdp, size_t len);
/** 生成非 host 本地候选的 SDP 行（srflx/relay），返回写入字节数。 */
size_t zcs_webrtc_ice_sdp_candidates(struct zcs_webrtc_ice *ice, char *out, size_t cap);
void zcs_webrtc_ice_on_udp(struct zcs_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len);
int zcs_webrtc_ice_send(struct zcs_webrtc_ice *ice, const void *data, size_t len);
/** 当前选中的本地候选是否为 TURN relay（媒体需走 TURN 发送）。 */
int zcs_webrtc_ice_uses_relay(struct zcs_webrtc_ice *ice);
int zcs_webrtc_ice_connected(const struct zcs_webrtc_ice *ice);
#endif /* ZCS_SRC_PROTOCOL_WEBRTC_ICE_INTERNAL_H */
