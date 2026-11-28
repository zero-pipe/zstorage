/**
 * @file sidecar.h
 * @brief DASH 录制 sidecar 装配。
 */
#ifndef ZCS_PLAY_DASH_SIDECAR_H
#define ZCS_PLAY_DASH_SIDECAR_H

#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"
#include "zn/poller/poller.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 为直播源确保已存在 DASH 录制器（VOD 源直接忽略）。
 * 内部用默认参数创建并绑定到 HLS 主 poller；形参 poller 当前未被使用，仅保留接口。
 */
ZCS_API void zcs_http_dash_ensure_recorder(zcs_media_source *src, zn_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_DASH_SIDECAR_H */
