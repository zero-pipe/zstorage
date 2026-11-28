/**
 * @file event.h
 * @brief 媒体事件与 tuple。
 */
#ifndef ZCS_CORE_EVENT_H
#define ZCS_CORE_EVENT_H

/**
 * 媒体生命周期事件（推流 / 播放 / 读者变更）。
 *
 * on_media_publish          流注册
 * on_media_publish_fini     流注销
 * on_media_play             开始播放
 * on_media_stop             停止播放
 * on_media_reader_changed   观众数变更
 * on_media_none_reader      无观众（触发延迟清理）
 */
#include "zcs/core/stream/limits.h"
#include <stdint.h>
#include "zcs/zcs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;

/** 前向声明；事件回调只需要源指针。 */
struct zcs_media_source;
typedef struct zcs_media_source zcs_media_source;

/** 服务监听端口（推流成功后日志 / API URL 打印）。 */
typedef struct zcs_media_server_ports {
    unsigned rtmp;
    unsigned rtsp;
    unsigned http;
    unsigned srt;
} zcs_media_server_ports;

/** 推流来源类型 */
typedef enum {
    ZCS_ORIGIN_UNKNOWN = 0,
    ZCS_ORIGIN_RTMP_PUSH = 1,
    ZCS_ORIGIN_RTSP_PUSH = 2,
    ZCS_ORIGIN_SRT_PUSH = 4,
    ZCS_ORIGIN_PULL = 8,
    ZCS_ORIGIN_MP4_VOD = 16,
    ZCS_ORIGIN_RTP_PS_PUSH = 32,
    ZCS_ORIGIN_WEBRTC_PUSH = 64,
} zcs_media_origin;

typedef struct zcs_media_tuple {
    char schema[ZCS_SCHEMA_MAX];
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
} zcs_media_tuple;

typedef void (*zcs_media_publish_cb)(const zcs_media_tuple *tuple, zcs_media_origin origin,
                                     void *user);
typedef void (*zcs_media_publish_fini_cb)(const zcs_media_tuple *tuple, zcs_media_origin origin,
                                          void *user);
typedef void (*zcs_media_play_cb)(const zcs_media_tuple *tuple, const char *player_schema,
                                  void *user);
typedef void (*zcs_media_stop_cb)(const zcs_media_tuple *tuple, const char *player_schema,
                                  void *user);
typedef void (*zcs_media_reader_changed_cb)(const zcs_media_tuple *tuple, int total_reader_count,
                                            void *user);
typedef void (*zcs_media_none_reader_cb)(const zcs_media_tuple *tuple, void *user);

typedef struct zcs_media_events {
    zcs_media_publish_cb on_media_publish;
    zcs_media_publish_fini_cb on_media_publish_fini;
    zcs_media_play_cb on_media_play;
    zcs_media_stop_cb on_media_stop;
    zcs_media_reader_changed_cb on_media_reader_changed;
    zcs_media_none_reader_cb on_media_none_reader;
    void *user;
} zcs_media_events;

/** 将来源枚举转为小写下划线字符串（如 "rtmp_push"、"rtsp_push"）。 */
ZCS_API const char *zcs_media_origin_str(zcs_media_origin origin);

/** 注册全局事件回调；poller 用于无观众延迟（可为 NULL）。 */
ZCS_API void zcs_media_events_set(zn_poller *poller, const zcs_media_events *events,
                                  int none_reader_delay_ms);
/** 取注册事件时带入的 poller（无观众延迟定时器的宿主）。 */
ZCS_API zn_poller *zcs_media_events_poller(void);

/** 断流后保留源的时长（ms）；0 表示立即清理。 */
ZCS_API void zcs_media_events_set_continue_push_ms(int ms);
/** 取当前断流保留时长（ms）；0 表示不保留。 */
ZCS_API int zcs_media_continue_push_ms(void);
/**
 * 推流结束时调用：返回 1 表示已进入 linger（暂不清理 GOP/不发 publish_fini），
 * 返回 0 表示调用方应继续原有清理流程。
 */
ZCS_API int zcs_media_source_linger(zcs_media_source *src, zcs_media_origin origin);
/** 新推流/主动关流时取消 linger 定时器。 */
ZCS_API void zcs_media_linger_cancel(const char *app, const char *stream);

/** 服务监听端口（推流成功后日志 / API URL 打印）。 */
ZCS_API void zcs_media_events_set_server_ports(const zcs_media_server_ports *ports);
/** 取已设置的监听端口（借用指针）；未设置返回 NULL。 */
ZCS_API const zcs_media_server_ports *zcs_media_events_server_ports(void);

/** 从媒体源提取 schema/vhost/app/stream 组成 tuple（输出清零后拷贝）；tuple 为 NULL 时空操作。 */
ZCS_API void zcs_media_tuple_from_source(const zcs_media_source *src, zcs_media_tuple *tuple);

/** 触发推流注册事件：置 publishing、打印访问地址、回调 on_media_publish 并推送 webhook。 */
ZCS_API void zcs_media_event_publish(zcs_media_source *src, zcs_media_origin origin);
/** 触发推流结束事件：若仍在推（publishing=1）则回调 on_media_publish_fini 并推送 webhook；幂等。 */
ZCS_API void zcs_media_event_publish_fini(zcs_media_source *src, zcs_media_origin origin);
/** 触发播放开始事件：回调 on_media_play。 */
ZCS_API void zcs_media_event_play(zcs_media_source *src, const char *player_schema);
/**
 * 通知出站会话已结束。
 * @param play_start_ms  播放开始的单调时间戳（zn_monotonic_ms()）；
 *                       未知则传 0（webhook 将省略 duration_ms）。
 */
ZCS_API void zcs_media_event_stop(zcs_media_source *src, const char *player_schema,
                                  uint64_t play_start_ms);

/** 观众附着/脱离（触发 totalReaderCount / onReaderChanged）。 */
ZCS_API void zcs_media_source_reader_add(zcs_media_source *src);
/** 观众脱离：reader_count 减一（已为 0 时忽略），并在归零后调度无观众检查。 */
ZCS_API void zcs_media_source_reader_remove(zcs_media_source *src);
/** 查询当前观众数（持注册表锁读取）；src 为 NULL 返回 0。 */
ZCS_API int zcs_media_source_reader_count(const zcs_media_source *src);

/** 取消待处理的无观众定时器；poller 池关停前调用。 */
ZCS_API void zcs_media_events_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_EVENT_H */
