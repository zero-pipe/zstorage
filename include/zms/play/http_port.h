#ifndef ZMS_PLAY_HTTP_PORT_H
#define ZMS_PLAY_HTTP_PORT_H

/**
 * @file http_port.h
 * @brief HTTP 流输出端口。
 *
 * app/http 的流输出路由（HLS/DASH/HTTP-FLV/HTTP-TS/VOD）只允许通过本文件
 * 进入 service/play 的实现，不得直接包含各角色内部头。实现在 service/play。
 */
#include "zms/play/dash/playlist.h"
#include "zms/play/dash/segmenter.h"
#include "zms/play/dash/sidecar.h"
#include "zms/play/hls/playlist.h"
#include "zms/play/hls/segmenter.h"
#include "zms/play/hls/sidecar.h"
#include "zms/play/http_flv/live_muxer.h"
#include "zms/vod/play/flv_muxer.h"
#include "zms/vod/play/hls.h"
#include "zms/vod/play/play_lane.h"
#include "zms/vod/io/flv_cache.h"
#include "zms/vod/io/source.h"

#endif /* ZMS_PLAY_HTTP_PORT_H */
