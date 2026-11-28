#ifndef ZCS_PLAY_HTTP_PORT_H
#define ZCS_PLAY_HTTP_PORT_H

/**
 * @file http_port.h
 * @brief HTTP 流输出端口。
 *
 * app/http 的流输出路由（HLS/DASH/HTTP-FLV/HTTP-TS/VOD）只允许通过本文件
 * 进入 service/play 的实现，不得直接包含各角色内部头。实现在 service/play。
 */
#include "zcs/play/dash/playlist.h"
#include "zcs/play/dash/segmenter.h"
#include "zcs/play/dash/sidecar.h"
#include "zcs/play/hls/playlist.h"
#include "zcs/play/hls/segmenter.h"
#include "zcs/play/hls/sidecar.h"
#include "zcs/play/http_flv/live_muxer.h"
#include "zcs/vod/play/flv_muxer.h"
#include "zcs/vod/play/hls.h"
#include "zcs/vod/play/play_lane.h"
#include "zcs/vod/io/flv_cache.h"
#include "zcs/vod/io/source.h"

#endif /* ZCS_PLAY_HTTP_PORT_H */
