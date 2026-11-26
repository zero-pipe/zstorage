/**
 * @file session_transport.h
 * @brief 协议 chunk/命令 → rtmp_srv_* 适配。
 */
#ifndef ZMS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H
#define ZMS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H

#include "zms/protocol/rtmp/protocol.h"
#include <stddef.h>
#include <stdint.h>

typedef struct zms_rtmp_session zms_rtmp_session;

/** 建立基于 zms_rtmp_protocol 的 RTMP 服务端会话传输。 */
ZMS_LOCAL zms_rtmp_protocol *zms_rtmp_transport_create(zms_rtmp_session *s);

/** 协议消息（媒体/命令）由 transport 分发给会话回调。 */
ZMS_LOCAL zn_err_t zms_rtmp_transport_input(zms_rtmp_session *s, const void *data, size_t len);

/** 媒体/脚本 tag 发送（type: 8/9/18）。 */
ZMS_LOCAL int zms_rtmp_session_send_media(zms_rtmp_session *s, uint8_t type, const void *body,
                                          size_t len, uint32_t tag_dts_ms);

/** 播放开始状态（StreamBegin + Play.Reset/Start 等），由 play 命令 dispatch 调用。 */
ZMS_LOCAL void zms_rtmp_transport_send_play_start(zms_rtmp_session *s, double transaction,
                                                  int reset);

/** publish 结果响应：ok 发送 Publish.Start，否则发送 BadAuth。 */
ZMS_LOCAL void zms_rtmp_transport_publish_result(zms_rtmp_session *s, double transaction, int ok);

#endif /* ZMS_SRC_PROTOCOL_RTMP_SESSION_TRANSPORT_H */
