/**
 * @file whip_ingress.h
 * @brief WHIP 推流入站。
 */
#ifndef ZCS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H
#define ZCS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H

#include "zn/zn_errno.h"
#include <stddef.h>

struct zcs_webrtc_session;

void zcs_webrtc_whip_ingress_on_udp(struct zcs_webrtc_session *s, const void *data, size_t len);
void zcs_webrtc_whip_ingress_stop(struct zcs_webrtc_session *s);

#endif /* ZCS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H */
