/**
 * @file sidecar.h
 * @brief HLS 录制 sidecar 装配。
 */
#ifndef ZCS_PLAY_HLS_SIDECAR_H
#define ZCS_PLAY_HLS_SIDECAR_H

/** HLS 全局 init：推流时自动切片，HTTP 同端口分发 m3u8/ts */
#include "zcs/core/event.h"
#include "zcs/zcs_export.h"
#include "zn/poller/poller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_hls_opts {
    int enable;
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
    /** 强制 fMP4（对应 ZLM enable_hls_fmp4）；0 时按 codec 自动选择 */
    int force_fmp4;
    int demand;
    int dash_demand;
    char save_path[256];
} zcs_http_hls_opts;

/** 填充 HLS 默认选项（enable=0，切片 2s×3，enable_audio=1）。 */
ZCS_API void zcs_http_hls_default_opts(zcs_http_hls_opts *opts);
/** 包装 media_events_set，在推流/断流时自动启停 HLS 切片 */
ZCS_API void zcs_http_hls_init(zn_poller *poller, const zcs_http_hls_opts *hls,
                               const zcs_media_events *events, int none_reader_delay_ms);
/** 关闭 HLS 切片子系统并复位全局状态、清空事件包装（与 init 配对）。 */
ZCS_API void zcs_http_hls_fini(void);

/** HLS 切片 timer 绑到 zcs_http_hls_init 的主 poller；@p poller 已忽略（E2-5） */
ZCS_API void zcs_http_hls_bind_recorder_timer(zcs_media_source *src, zn_poller *poller);
/** @return init 时绑定用于 HLS 切片定时器的主 poller；未初始化返回 NULL。 */
ZCS_API zn_poller *zcs_http_hls_main_poller(void);
/** 直播 HLS 懒创建；timer 始终绑主 poller，@p poller 已忽略 */
ZCS_API void zcs_http_hls_ensure_recorder(zcs_media_source *src, zn_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_HLS_SIDECAR_H */
