/**
 * @file segmenter.h
 * @brief 直播 DASH 分片器。
 */
#ifndef ZCS_PLAY_DASH_SEGMENTER_H
#define ZCS_PLAY_DASH_SEGMENTER_H

/**
 * Live DASH sidecar：gop_queue → libdash dash_mpd + fMP4 分片（内存环）。
 */
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"
#include "zn/poller/poller.h"
#include "zn/util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_dash_segmenter_opts {
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
} zcs_http_dash_segmenter_opts;

typedef struct zcs_http_dash_playlist zcs_http_dash_playlist;
typedef struct zcs_http_dash_segmenter zcs_http_dash_segmenter;

/** 填充默认参数：分片 2 秒、保留 24 片、包含音频；opts 为 NULL 时无操作。 */
ZCS_API void zcs_http_dash_segmenter_default_opts(zcs_http_dash_segmenter_opts *opts);
/**
 * 为直播源创建 DASH 分片器（要求 src 有 gop_queue）。
 * opts 为 NULL 时用默认参数；创建后会自动附着到 src 的 dash sidecar 槽。
 * @return 分片器实例；src/opts 非法或分配失败返回 NULL。用 destroy 释放。
 */
ZCS_API zcs_http_dash_segmenter *
zcs_http_dash_segmenter_create(zcs_media_source *src, const zcs_http_dash_segmenter_opts *opts);
/** 释放分片器（停 timer、销毁 reader/MPD/缓冲）并摘除 sidecar；对 NULL 安全。 */
ZCS_API void zcs_http_dash_segmenter_destroy(zcs_http_dash_segmenter *rec);
/** 取内部 MPD 分片列表（借用，随实例销毁失效）；rec 为 NULL 返回 NULL。 */
ZCS_API zcs_http_dash_playlist *zcs_http_dash_segmenter_playlist(zcs_http_dash_segmenter *rec);
/** 在 poller 上挂周期 tick 推进分片；内部持互斥保护 libdash 时间轴。 */
ZCS_API void zcs_http_dash_segmenter_bind_timer(zcs_http_dash_segmenter *rec, zn_poller *poller);
/** HTTP 服务期间暂停 timer（depth>0）；结束后仅在主 poller 上恢复 */
ZCS_API void zcs_http_dash_segmenter_http_enter(zcs_http_dash_segmenter *rec);
/** 结束 HTTP 服务（depth 递减），归零后仅在主 poller（poller 参数）上恢复 timer。 */
ZCS_API void zcs_http_dash_segmenter_http_leave(zcs_http_dash_segmenter *rec, zn_poller *poller);
/** 同步推进一次分片（持锁，可与 timer tick 并发安全）；rec 为 NULL 时无操作。 */
ZCS_API void zcs_http_dash_segmenter_tick(zcs_http_dash_segmenter *rec);
/** 持锁 pump + 生成 MPD，避免与 timer tick 并发更新 libdash 时间轴 */
ZCS_API zn_err_t zcs_http_dash_segmenter_serve_mpd(zcs_http_dash_segmenter *rec, char *out,
                                                    size_t cap, size_t *out_len, int max_ticks,
                                                    int *has_media);
/** 持锁 pump + 拷贝分片 */
ZCS_API zn_err_t zcs_http_dash_segmenter_copy_segment(zcs_http_dash_segmenter *rec,
                                                       const char *name, uint8_t *buf, size_t cap,
                                                       size_t *out_len, int max_ticks);
/**
 * 持锁 pump，按 name 取得分片引用（零拷贝）。
 * @param out_buf 输出分片缓冲引用，需由调用方释放
 * @param max_ticks 追帧 pump 上限；<=0 取默认值
 * @return ZN_OK 且 *out_len>0 表示命中；未命中返回 ZN_ERR_INVALID 且 *out_len=0。
 */
ZCS_API zn_err_t zcs_http_dash_segmenter_ref_segment(zcs_http_dash_segmenter *rec,
                                                      const char *name, zn_buf **out_buf,
                                                      size_t *out_len, int max_ticks);

/** 取 DASH 内建 ops 表（静态，借用指针，无需释放），供注册表注册。 */
ZCS_API const zcs_segment_recorder_ops *zcs_http_dash_segment_recorder_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_DASH_SEGMENTER_H */
