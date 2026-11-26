#ifndef ZMS_PROTOCOL_H
#define ZMS_PROTOCOL_H

/**
 * @file protocol.h
 * @brief RTSP / RTP / RTMP / HTTP / SRT 协议栈聚合头。
 */
#include "zms/core/frame.h"
#include "zms/core/media_track.h"
#include "zms/core/codec_filter.h"
#include "zms/play/dispatch.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/media/wire/rtcp.h"
#include "zms/media/wire/rtp_receiver.h"
#include "zms/protocol/rtsp/rtsp.h"
#include "zms/protocol/rtsp/parser.h"
#include "zms/protocol/rtsp/splitter.h"
#include "zms/protocol/rtsp/sdp.h"
#include "zms/protocol/rtsp/client.h"
#include "zms/media/wire/rtmp.h"
#include "zms/media/wire/amf.h"
#include "zms/protocol/rtmp/protocol.h"
#include "zms/protocol/rtmp/client.h"
#include "zms/protocol/http/request_reader.h"
#include "zms/protocol/http/flv_client.h"
#include "zms/protocol/http/hls_client.h"
#include "zms/protocol/srt/srt_server.h"
#include "zms/protocol/srt/streamid.h"

/* 服务端会话接口（*_server.h / session.h / session_auth.h / whep.h）已归位到
 * zms/session.h 聚合头；本头只保留纯线协议编解码。 */

#endif /* ZMS_PROTOCOL_H */
