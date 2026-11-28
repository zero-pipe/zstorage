/**
 * @file play_session.h
 * @brief WebRTC 播放会话。
 */
#ifndef ZCS_SRC_SESSION_WEBRTC_PLAY_SESSION_H
#define ZCS_SRC_SESSION_WEBRTC_PLAY_SESSION_H

#include "zn/zn_errno.h"
#include <stddef.h>

struct zcs_webrtc_session;

zn_err_t zcs_webrtc_play_on_stun_dtls(struct zcs_webrtc_session *s, const void *data, size_t len);
void zcs_webrtc_play_input(struct zcs_webrtc_session *s, const void *data, size_t len);
void zcs_webrtc_play_stop(struct zcs_webrtc_session *s);

#endif /* ZCS_SRC_SESSION_WEBRTC_PLAY_SESSION_H */
