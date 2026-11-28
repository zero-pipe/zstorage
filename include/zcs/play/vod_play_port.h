#ifndef ZCS_PLAY_VOD_PLAY_PORT_H
#define ZCS_PLAY_VOD_PLAY_PORT_H

/**
 * @file vod_play_port.h
 * @brief 点播播放端口。
 *
 * app 协议层（HTTP/RTSP/RTMP）需要点播源、reader、play lane 与 VOD muxer
 * 时只包含本文件，不直接依赖 vod/io 或 vod/play 的内部头。
 * 实现在 vod/io 与 vod/play。
 */
#include "zcs/play/http_flv/live_muxer.h"
#include "zcs/vod/play/flv_muxer.h"
#include "zcs/vod/play/play_lane.h"
#include "zcs/vod/io/reader.h"
#include "zcs/vod/io/source.h"

#endif /* ZCS_PLAY_VOD_PLAY_PORT_H */
