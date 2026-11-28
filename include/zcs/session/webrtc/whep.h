/**
 * @file whep.h
 * @brief WHEP（WebRTC 播放）。
 */
#ifndef ZCS_SESSION_WEBRTC_WHEP_H
#define ZCS_SESSION_WEBRTC_WHEP_H

#include "zcs/zcs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_http_session;
struct zcs_http_request;

/** WHEP / ZLM webrtc 播放：POST offer SDP，回 201 + answer SDP。DELETE Location 资源以拆除。 */
ZCS_API void zcs_webrtc_whep_handle(struct zcs_http_session *hs,
                                    const struct zcs_http_request *req);

/** WHIP 推流：POST offer SDP，回 201 + answer SDP。DELETE Location 资源以拆除。 */
ZCS_API void zcs_webrtc_whip_handle(struct zcs_http_session *hs,
                                    const struct zcs_http_request *req);

void zcs_webrtc_whep_routes_register(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SESSION_WEBRTC_WHEP_H */
