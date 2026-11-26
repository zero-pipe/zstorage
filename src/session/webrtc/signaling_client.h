/**
 * @file signaling_client.h
 * @brief 外部 WebSocket 信令客户端：设备作为 master，浏览器 viewer 发 offer。
 */
#ifndef ZMS_SRC_SESSION_WEBRTC_SIGNALING_CLIENT_H
#define ZMS_SRC_SESSION_WEBRTC_SIGNALING_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int zms_webrtc_signaling_start(const char *url, const char *room, const char *token,
                               const char *app, const char *stream);
void zms_webrtc_signaling_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_SESSION_WEBRTC_SIGNALING_CLIENT_H */
