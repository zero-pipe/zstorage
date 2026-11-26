#ifndef ZMS_SESSION_H
#define ZMS_SESSION_H

/**
 * @file session.h
 * @brief 服务端会话聚合头：各协议的 server / session / 鉴权 / WHEP 接口。
 *
 * 与 zms/protocol.h（纯线协议编解码）成对：本头只聚合会话层对外接口，
 * 对应实现位于 src/session/<proto>/。
 */
#include "zms/session/http/http_server.h"
#include "zms/session/rtmp/rtmp_server.h"
#include "zms/session/rtmp/session.h"
#include "zms/session/rtsp/rtsp_server.h"
#include "zms/session/rtsp/session_auth.h"
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/session/webrtc/whep.h"

#endif /* ZMS_SESSION_H */
