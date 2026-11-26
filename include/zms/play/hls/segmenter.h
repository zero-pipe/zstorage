/**
 * @file segmenter.h
 * @brief 直播 HLS(TS) 分片器。
 */
#ifndef ZMS_PLAY_HLS_SEGMENTER_H
#define ZMS_PLAY_HLS_SEGMENTER_H

/**
 * Live HLS 录制：从 gop_queue ES（H.264 Annex-B / AAC）→ TS 切片。
 * 点播 HLS 请用 vod/play/hls.c，不得使用本模块。
 */
#include "zms/core/stream/hub.h"
#include "zms/play/hls/playlist.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

struct zn_poller;
typedef struct zn_poller zn_poller;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_http_hls_segmenter_opts {
    float segment_duration_sec;
    unsigned segment_count;
    int enable_audio;
    int force_fmp4;
    const char *save_path;
} zms_http_hls_segmenter_opts;

typedef struct zms_http_hls_segmenter zms_http_hls_segmenter;

/**
 * 填充默认分片选项：段时长 2s、保留 3 段、启用音频、不强制 fMP4。
 * 调用方可在 create 前覆盖字段。opts 为 NULL 时空操作。
 */
ZMS_API void zms_http_hls_segmenter_default_opts(zms_http_hls_segmenter_opts *opts);

/** 直播推流开始时创建；关流时 destroy。VOD 返回 NULL */
ZMS_API zms_http_hls_segmenter *
zms_http_hls_segmenter_create(zms_media_source *src, const zms_http_hls_segmenter_opts *opts);
/** 停止 timer、解绑 source 上的 recorder、flush 残余分片并释放全部资源。对 NULL 安全。 */
ZMS_API void zms_http_hls_segmenter_destroy(zms_http_hls_segmenter *rec);

/** poller manager HTTP 定时路径调用，从 ring 拉取并切片 */
ZMS_API void zms_http_hls_segmenter_tick(zms_http_hls_segmenter *rec);

/** HTTP 服务路径：持锁期间拉 ring（调用方须已 zms_http_hls_segmenter_lock） */
ZMS_API void zms_http_hls_segmenter_tick_locked(zms_http_hls_segmenter *rec);

/** 获取分片器内部互斥量（必须与 unlock 成对）。内部持有 rec->mu。 */
ZMS_API void zms_http_hls_segmenter_lock(zms_http_hls_segmenter *rec);
/** 释放分片器互斥量；必须与 lock 成对，不得跨线程配对。 */
ZMS_API void zms_http_hls_segmenter_unlock(zms_http_hls_segmenter *rec);

/** 取内部 playlist（借用，归属分片器）；rec 处于 closing 时返回 NULL。 */
ZMS_API zms_http_hls_playlist *zms_http_hls_segmenter_playlist(zms_http_hls_segmenter *rec);
/**
 * 绑定 timer 宿主 poller 并启动 10ms 定时 tick（实际优先用 HLS 主 poller，poller 为回退）。
 * 关闭途中只记录 poller 不启停 timer，避免与 destroy 竞争。
 */
ZMS_API void zms_http_hls_segmenter_bind_timer(zms_http_hls_segmenter *rec, zn_poller *poller);

/** HTTP 服务期间暂停 timer（depth>0）；结束后在 poller 上恢复 */
ZMS_API void zms_http_hls_segmenter_http_enter(zms_http_hls_segmenter *rec);
/** 退出 HTTP 服务时段：drain 深度减一，归零后在 poller 上恢复 timer。 */
ZMS_API void zms_http_hls_segmenter_http_leave(zms_http_hls_segmenter *rec);

/** 持锁 pump + 生成 m3u8，避免与 timer tick 并发写 ts mux / playlist */
ZMS_API zn_err_t zms_http_hls_segmenter_serve_m3u8(zms_http_hls_segmenter *rec, char *out,
                                                    size_t cap, size_t *out_len, int max_ticks,
                                                    int *seg_count);
/** 持锁 pump + 拷贝分片 */
ZMS_API zn_err_t zms_http_hls_segmenter_copy_segment(zms_http_hls_segmenter *rec, const char *name,
                                                      uint8_t *buf, size_t cap, size_t *out_len,
                                                      int max_ticks);
/**
 * 持锁 pump 后引用分片（零拷贝）：命中则 zn_buf_ref 给调用方，须由调用方 zn_buf_unref。
 * 最初没有该分片时最多 pump max_ticks 次等待产出。
 * @param max_ticks <=0 时取内部默认值
 * @return ZN_OK 命中并已 ref；ZN_ERR_INVALID 参数/状态非法或超时未产出（*out_len=0）
 */
ZMS_API zn_err_t zms_http_hls_segmenter_ref_segment(zms_http_hls_segmenter *rec, const char *name,
                                                     zn_buf **out_buf, size_t *out_len,
                                                     int max_ticks);

/** 记录 HLS 请求时间（live 滑动窗口保活） */
ZMS_API void zms_http_hls_segmenter_touch_hls(zms_http_hls_segmenter *rec);

/** segment_recorder 插件 ops */
ZMS_API const struct zms_segment_recorder_ops *zms_http_hls_segment_recorder_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PLAY_HLS_SEGMENTER_H */
