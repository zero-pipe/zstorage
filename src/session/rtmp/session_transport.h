/**
 * @file session_transport.h
 * @brief 协议 chunk/命令 → rtmp_srv_* 适配。
 */
#ifndef ZCS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H
#define ZCS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H

#include "zcs/protocol/rtmp/protocol.h"
#include <stddef.h>
#include <stdint.h>

typedef struct zcs_rtmp_session zcs_rtmp_session;

/** 建立基于 zcs_rtmp_protocol 的 RTMP 服务端会话传输。 */
ZCS_LOCAL zcs_rtmp_protocol *zcs_rtmp_transport_create(zcs_rtmp_session *s);

/** 协议消息（媒体/命令）由 transport 分发给会话回调。 */
ZCS_LOCAL zn_err_t zcs_rtmp_transport_input(zcs_rtmp_session *s, const void *data, size_t len);

/** 媒体/脚本 tag 发送（type: 8/9/18）。 */
ZCS_LOCAL int zcs_rtmp_session_send_media(zcs_rtmp_session *s, uint8_t type, const void *body,
                                          size_t len, uint32_t tag_dts_ms);

/** 播放开始状态（StreamBegin + Play.Reset/Start 等），由 play 命令 dispatch 调用。 */
ZCS_LOCAL void zcs_rtmp_transport_send_play_start(zcs_rtmp_session *s, double transaction,
                                                  int reset);

/** publish 结果响应：ok 发送 Publish.Start，否则发送 BadAuth。 */
ZCS_LOCAL void zcs_rtmp_transport_publish_result(zcs_rtmp_session *s, double transaction, int ok);

#endif /* ZCS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H */
