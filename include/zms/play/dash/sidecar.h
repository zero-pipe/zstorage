/**
 * @file sidecar.h
 * @brief DASH 录制 sidecar 装配。
 */
#ifndef ZMS_PLAY_DASH_SIDECAR_H
#define ZMS_PLAY_DASH_SIDECAR_H

#include "zms/core/stream/hub.h"
#include "zms/zms_export.h"
#include "zn/poller/poller.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 为直播源确保已存在 DASH 录制器（VOD 源直接忽略）。
 * 内部用默认参数创建并绑定到 HLS 主 poller；形参 poller 当前未被使用，仅保留接口。
 */
ZMS_API void zms_http_dash_ensure_recorder(zms_media_source *src, zn_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PLAY_DASH_SIDECAR_H */
