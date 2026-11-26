/**
 * @file play_session.h
 * @brief WebRTC 播放会话。
 */
#ifndef ZMS_SRC_SESSION_WEBRTC_PLAY_SESSION_H
#define ZMS_SRC_SESSION_WEBRTC_PLAY_SESSION_H

#include "zn/zn_errno.h"
#include <stddef.h>

struct zms_webrtc_session;

zn_err_t zms_webrtc_play_on_stun_dtls(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_play_input(struct zms_webrtc_session *s, const void *data, size_t len);
void zms_webrtc_play_stop(struct zms_webrtc_session *s);

#endif /* ZMS_SRC_SESSION_WEBRTC_PLAY_SESSION_H */
