#ifndef ZCS_PROTOCOL_H
#define ZCS_PROTOCOL_H

/**
 * @file protocol.h
 * @brief RTSP / RTP / RTMP / HTTP / SRT 协议栈聚合头。
 */
#include "zcs/core/frame.h"
#include "zcs/core/media_track.h"
#include "zcs/core/codec_filter.h"
#include "zcs/play/dispatch.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zcs/media/wire/rtcp.h"
#include "zcs/media/wire/rtp_receiver.h"
#include "zcs/protocol/rtsp/rtsp.h"
#include "zcs/protocol/rtsp/parser.h"
#include "zcs/protocol/rtsp/splitter.h"
#include "zcs/protocol/rtsp/sdp.h"
#include "zcs/protocol/rtsp/client.h"
#include "zcs/media/wire/rtmp.h"
#include "zcs/media/wire/amf.h"
#include "zcs/protocol/rtmp/protocol.h"
#include "zcs/protocol/rtmp/client.h"
#include "zcs/protocol/http/request_reader.h"
#include "zcs/protocol/http/flv_client.h"
#include "zcs/protocol/http/hls_client.h"

/* 服务端会话接口（*_server.h / session.h / session_auth.h / whep.h）已归位到
 * zcs/session.h 聚合头；本头只保留纯线协议编解码。 */

#endif /* ZCS_PROTOCOL_H */
