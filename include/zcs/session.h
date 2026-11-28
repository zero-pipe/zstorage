#ifndef ZCS_SESSION_H
#define ZCS_SESSION_H

/**
 * @file session.h
 * @brief 服务端会话聚合头：各协议的 server / session / 鉴权 / WHEP 接口。
 *
 * 与 zcs/protocol.h（纯线协议编解码）成对：本头只聚合会话层对外接口，
 * 对应实现位于 src/session/<proto>/。
 */
#include "zcs/session/http/http_server.h"
#include "zcs/session/rtmp/rtmp_server.h"
#include "zcs/session/rtmp/session.h"
#include "zcs/session/rtsp/rtsp_server.h"
#include "zcs/session/rtsp/session_auth.h"

#endif /* ZCS_SESSION_H */
