#ifndef ZMS_EGRESS_RECORDER_EGRESS_SEGMENT_RECORDER_H
#define ZMS_EGRESS_RECORDER_EGRESS_SEGMENT_RECORDER_H

/**
 * @file egress_segment_recorder.h
 * @brief 直播分片 sidecar 注册表（HLS / DASH）。
 *
 * 附着于 gop_queue。VOD 切片位于 vod/play（如 vod/play/hls.c）。
 */
#include "zms/core/stream/hub.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

struct zn_poller;
typedef struct zn_poller zn_poller;

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_SEGMENT_REC_HLS "hls"
#define ZMS_SEGMENT_REC_DASH "dash"

/** 每次 HTTP 服务 pump 的最大 tick 数（缩短持锁时间）。 */
#define ZMS_SEGMENT_REC_SERVE_MAX_TICKS 8
/** 每个 recorder 定时 tick 从 gop_queue mux 的帧数。 */
#define ZMS_SEGMENT_REC_TICK_FRAMES 32

typedef struct zms_segment_recorder_ops {
    const char *name;
    /** 仅直播；VOD 须返回 ZN_ERR_INVALID。 */
    zn_err_t (*create_live)(zms_media_source *src, const void *opts, void **out_rec);
    void (*destroy)(void *rec);
    void (*bind_timer)(void *rec, zn_poller *poller);
    void (*tick)(void *rec);
    /** HTTP 拉流保活（滑动窗口）。 */
    void (*touch)(void *rec);
} zms_segment_recorder_ops;

/**
 * 注册一种分段录制器 ops。同名会替换旧表，容量上限 8。
 * 注意：内部只保存指针，ops 必须具有静态/进程级生命周期，不能在栈上临时构造。
 */
ZMS_API void zms_segment_recorder_register(const zms_segment_recorder_ops *ops);
/** 注册内建 HLS/DASH ops — 仅由 zms_modules_register_all 调用。 */
ZMS_API void zms_segment_recorder_register_builtins(void);

/** 按名字查找已注册的 ops；返回借用指针，未注册返回 NULL。 */
ZMS_API const zms_segment_recorder_ops *zms_segment_recorder_find_ops(const char *name);

/** 取 src 上名为 name 的录制器实例（借用指针）；未附着返回 NULL。 */
ZMS_API void *zms_media_source_segment_rec_get(const zms_media_source *src, const char *name);
/**
 * 在 src 上附着/解除名为 name 的录制器实例。
 * @param rec 非 NULL 时附着（不接管释放，销毁仍由调用方负责）；NULL 时摘除该条目。
 * @return name 过长或槽位耗尽返回错误；src/name 非法返回 ZN_ERR_INVALID。
 */
ZMS_API zn_err_t zms_media_source_segment_rec_set(zms_media_source *src, const char *name,
                                                   void *rec);

/**
 * 为直播源按 name 创建录制器（首次创建）。
 * 仅直播：VOD 源或缺 gop_queue 返回 ZN_ERR_INVALID；已存在则直接返回 ZN_OK（幂等）。
 * @param opts 传给 ops->create_live 的创建参数，按 name 类型解释。
 */
ZMS_API zn_err_t zms_segment_recorder_create_live(zms_media_source *src, const char *name,
                                                   const void *opts);
/** 销毁并摘除 src 上的指定直播录制器；不存在时无操作。 */
ZMS_API void zms_segment_recorder_destroy_live(zms_media_source *src, const char *name);
/** 销毁 src 上所有已附着的直播录制器（如停推清理）。 */
ZMS_API void zms_segment_recorder_destroy_all(zms_media_source *src);

/**
 * 保证 src 上已有指定录制器，并绑定 poller 定时器；不存在则先创建。
 * 组合根注册内建 ops 后使用；返回最终是否可用（创建失败或绑定后仍不存在则为错误）。
 */
ZMS_API zn_err_t zms_segment_recorder_ensure_live(zms_media_source *src, const char *name,
                                                   zn_poller *poller, const void *opts);
/** 为已存在的录制器绑定 poller 定时 tick；实例或 poller 缺失则无操作。 */
ZMS_API void zms_segment_recorder_bind_timer(zms_media_source *src, const char *name,
                                             zn_poller *poller);
/** 触发一次录制器定时 tick（从 gop_queue 拉取帧做分片）；实例不存在则无操作。 */
ZMS_API void zms_segment_recorder_tick(zms_media_source *src, const char *name);
/** HTTP 拉流保活：前移录制器的活动窗口；实例不存在则无操作。 */
ZMS_API void zms_segment_recorder_touch(zms_media_source *src, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_RECORDER_EGRESS_SEGMENT_RECORDER_H */
