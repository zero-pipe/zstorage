/**
 * @file whip_ingress.h
 * @brief WHIP 推流入站。
 */
#ifndef ZMS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H
#define ZMS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H

#include "zn/zn_errno.h"
#include <stddef.h>

struct zms_webrtc_session;

void zms_webrtc_whip_ingress_on_udp(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_whip_ingress_stop(struct zms_webrtc_session *s);

#endif /* ZMS_SRC_SESSION_WEBRTC_WHIP_INGRESS_H */
