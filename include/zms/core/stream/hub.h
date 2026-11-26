#ifndef ZMS_CORE_STREAM_HUB_H
#define ZMS_CORE_STREAM_HUB_H

/**
 * @file hub.h
 * @brief 流注册表：app/stream 标识、轨道与共享缓冲。
 *
 * 直播 source 拥有 @ref zms_gop_queue；点播 source 使用 @ref zms_vod_buffer。
 * 同一 source 上二者互斥。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/event.h"
#include "zms/core/stream/limits.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/stats.h"
#include "zms/core/media_track.h"
#include "zms/core/stream_track.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>

struct zms_vod_buffer;

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_SCHEMA_RTMP "rtmp"
#define ZMS_SCHEMA_RTSP "rtsp"
#define ZMS_SCHEMA_SRT "srt"
#define ZMS_SCHEMA_RTP_PS "rtp-ps"

typedef struct zms_media_source {
    char schema[ZMS_SCHEMA_MAX];
    char vhost[ZMS_VHOST_MAX];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    /** 客户端推流使用的 stream 名（register_publish 时与 stream 不同） */
    char stream_requested[ZMS_STREAM_MAX];
    zms_gop_queue *gop_queue;
    struct zms_vod_buffer *vod_buffer;
    int has_video;
    int has_audio;
    zms_video_track video;
    zms_audio_track audio;
    int reader_count;
    int vod_play_lane_ref;
    int publishing;
    int publish_origin;
    uint64_t create_stamp_ms;
    zms_media_stream_stats stats;
    void *publisher_ctx;
    void (*publisher_kick)(void *ctx, int force);
    /** 直播切片 sidecar 袋（HLS/DASH）；由 egress_segment_recorder 持有。 */
    void *segment_sidecar;
    /** on_publish 响应 enable_mp4；非 0 时启动直播 MP4 录制器 */
    int enable_mp4;
    /** 直播 MP4 录制器（egress_mp4_recorder）；停推时 finalize */
    void *mp4_recorder;
    /** 录制器 attach 时注册的停止回调；core 停推时无需认识 egress 头 */
    void (*mp4_recorder_stop)(zms_media_source *src);
    /** 云存储录制器（egress_zs_recorder）；断流时封口 */
    void *zs_recorder;
    /** 云存储录制器的停止回调（同上，core 不认识存储层） */
    void (*zs_recorder_stop)(zms_media_source *src);
} zms_media_source;

typedef struct zms_media_source_filter {
    const char *schema;
    const char *vhost;
    const char *app;
    const char *stream;
} zms_media_source_filter;

typedef int (*zms_media_source_visit_fn)(zms_media_source *src, void *user);

/** vhost 维度开关（[general] enableVhost）；关闭时所有 host 归一化为默认 vhost。 */
ZMS_API void zms_vhost_set_enabled(int enable);
/** 查询 vhost 维度开关当前是否启用（[general] enableVhost）。 */
ZMS_API int zms_vhost_enabled(void);
/** 解析/归一化主机名：去端口、去 scheme、转小写；未启用或为空则返回默认 vhost。 */
ZMS_API void zms_vhost_normalize(const char *in, char *out, size_t cap);

/** 初始化注册表与互斥量。进程启动时调用一次；重复调用无副作用。 */
ZMS_API void zms_media_source_registry_init(void);
/** poller 线程下保护注册表与 reader_count；media 模块内部使用 */
ZMS_API void zms_media_source_registry_lock(void);
/** 释放注册表锁。必须与 registry_lock 成对；不得跨线程配对。 */
ZMS_API void zms_media_source_registry_unlock(void);
/** 当前已注册的媒体源数量（含点播）。调用方需自行持锁或确保在 poller 线程。 */
ZMS_API int zms_media_source_count(void);
/**
 * 按 schema/app/stream 查找（默认 vhost）。
 * @return 命中的源（借用，不转移所有权）；未找到返回 NULL。
 */
ZMS_API zms_media_source *zms_media_source_find(const char *schema, const char *app,
                                                const char *stream);
/**
 * 同 zms_media_source_find，但显式指定 vhost。
 * @param vhost 为 NULL 或空串时按默认 vhost 处理（会先归一化）。
 */
ZMS_API zms_media_source *zms_media_source_find_ex(const char *schema, const char *vhost,
                                                   const char *app, const char *stream);
/** API 查流：schema=rtsp 时回退到 rtmp 枢纽 */
ZMS_API zms_media_source *zms_media_source_find_api(const char *schema, const char *app,
                                                    const char *stream);
/**
 * 同 zms_media_source_find_api，但显式指定 vhost。
 * WebAPI 查询入口使用本函数，保证多租户下查到的源与推流侧一致。
 */
ZMS_API zms_media_source *zms_media_source_find_api_ex(const char *schema, const char *vhost,
                                                       const char *app, const char *stream);
/** 播放查流（直播：等价于 zms_media_source_find_api） */
ZMS_API zms_media_source *zms_media_source_find_for_play(const char *schema, const char *app,
                                                         const char *stream);
/**
 * 播放查流的完整版本（带 vhost），是各协议播放入口的实际调用点。
 *
 * 查找顺序：先按 schema/app/stream 精确查；未命中则按点播文件规则按需打开；
 * 最后对 app=live 额外尝试 stream/<id> 等兼容写法。
 *
 * @return 可用于播放的源（可能因本次调用而新建）；未命中返回 NULL。
 */
ZMS_API zms_media_source *zms_media_source_find_for_play_ex(const char *schema, const char *vhost,
                                                            const char *app, const char *stream);
/**
 * 遍历所有已注册的媒体源（持锁调用）。
 *
 * @param fn     回调；返回非 0 时提前终止遍历
 * @param filter 字段级过滤；NULL 表示全部遍历；某字段为 NULL 表示该维度不限
 *
 * 注意：回调运行在持锁状态，回调内不得再次调用注册表 API（会自锁）。
 */
ZMS_API void zms_media_source_foreach(zms_media_source_visit_fn fn, void *user,
                                      const zms_media_source_filter *filter);
/**
 * 绑定推流者上下文与“踢出”回调。
 *
 * kick 用于同名流被新推流抢占时通知旧推流者退出；force=1 表示立即断开。
 * 停推路径上 core 不认识具体协议，靠这个回调解耦。
 */
ZMS_API void zms_media_source_set_publisher(zms_media_source *s, void *ctx,
                                            void (*kick)(void *ctx, int force));
/**
 * 解除推流者绑定；仅当当前 ctx 与传入 ctx 一致时才清除，避免旧会话误清新会话。
 */
ZMS_API void zms_media_source_clear_publisher(zms_media_source *s, void *ctx);
/**
 * 关闭一路源：唤醒读者、销毁缓冲、从注册表摘除。
 * @param force 1=立即关闭（无视仍存在的读者）；0=按常规流程
 * @return 1 表示已关闭；0 表示因仍有读者而延后
 */
ZMS_API int zms_media_source_close(zms_media_source *s, int force);
/** 精确 stream 名注册（代理/测试）；同名校验并 kick 旧推流 */
ZMS_API zms_media_source *zms_media_source_register(const char *schema, const char *app,
                                                    const char *stream);
/**
 * 精确注册（带 vhost）。同名已存在时返回同一实例而非新建。
 * @return 新建或已存在的源；失败返回 NULL。
 */
ZMS_API zms_media_source *zms_media_source_register_ex(const char *schema, const char *vhost,
                                                       const char *app, const char *stream);
/** 点播注册：仅 vod_buffer，无 gop_queue */
ZMS_API zms_media_source *zms_media_source_register_vod(const char *schema, const char *app,
                                                        const char *stream);
/**
 * 点播注册（带 vhost）。与直播源的区别：只建 vod_buffer，不建 gop_queue。
 * 生命周期由打开该文件的读取器持有；关闭时调用 unregister_vod。
 */
ZMS_API zms_media_source *zms_media_source_register_vod_ex(const char *schema, const char *vhost,
                                                           const char *app, const char *stream);
/**
 * 注销点播源并释放其 vod_buffer。
 * 必须在没有读者时才调用（读取器销毁流程的最后一步）。
 */
ZMS_API void zms_media_source_unregister_vod(zms_media_source *s);
/** 推流注册：在 stream 后自动加 _id，不抢占同路流 */
ZMS_API zms_media_source *zms_media_source_register_publish(const char *schema, const char *app,
                                                            const char *stream_requested);
/**
 * 推流注册（带 vhost）。
 *
 * 与 register 的差别：实际 stream 名会追加唯一的 _id 后缀，因此同一路流
 * 允许新旧两个推流会话同时存在片刻，由 kick 回调决定谁被踢出。
 *
 * @param stream_requested 客户端请求的 stream 名（原样记录便于对外展示）
 */
ZMS_API zms_media_source *zms_media_source_register_publish_ex(
    const char *schema, const char *vhost, const char *app, const char *stream_requested);
/**
 * 清空源的媒体数据（缓冲、轨道信息、统计），保留注册表条目与标识。
 * 用于推流重连：新推流复用同一源时先清掉上一轮的残留数据。
 */
ZMS_API void zms_media_source_clear(zms_media_source *s);
/**
 * 从注册表摘除并释放该源（含 gop_queue）。
 * 调用前必须确保：推流已停止、所有读者已 detach。
 */
ZMS_API void zms_media_source_unregister(zms_media_source *s);
/**
 * 以 info 级别打印当前注册表内容（调试/启动自检用）。
 * @param ports 端口信息，用于附注各协议的访问地址；可为 NULL
 */
ZMS_API void zms_media_source_log_registry(const zms_media_server_ports *ports);

/** 播放 config：来自 gop_queue */
/**
 * 取视频参数集（H.264 为 SPS+PPS；H.265 为 VPS+SPS+PPS）。
 * @param len 输出长度
 * @return 借用的指针（归属源，随源销毁失效）；无配置返回 NULL
 */
ZMS_API const uint8_t *zms_media_source_video_config(const zms_media_source *s, size_t *len);
/**
 * 取音频配置（AAC 为 AudioSpecificConfig；G.711/Opus 无独立配置，返回 NULL）。
 * @param len 输出长度
 * @return 借用的指针，随源销毁失效
 */
ZMS_API const uint8_t *zms_media_source_audio_config(const zms_media_source *s, size_t *len);

/** 播放是否可走 gop_queue（H.264/H.265 + AAC） */
ZMS_API int zms_media_source_use_gop_queue_play(const zms_media_source *s);

/** 媒体源是否为已注册的文件点播源（vod_buffer + MP4_VOD origin）。 */
ZMS_API int zms_media_source_is_vod(const zms_media_source *s);

/** 播放订阅者（live=gop_queue reader；vod=vod_buffer reader） */
typedef struct zms_media_subscriber {
    zms_gop_reader *gop;
    struct zms_vod_buffer_reader *vod;
} zms_media_subscriber;

/**
 * 解除播放订阅并释放 reader。
 * 幂等：对已 detach 或全空的订阅调用是安全的。
 */
ZMS_API void zms_media_subscriber_detach(zms_media_subscriber *r);
/** @param seek_live 1=live 边缘（write-48，可能无 IDR）；0=从头（VOD） */
ZMS_API zn_err_t zms_media_source_subscribe(zms_media_source *s, int seek_live,
                                             zms_media_subscriber *out);
/** 直播秒开：subscribe + seek_gop_key（RTMP/HTTP-FLV/RTSP seek_live 区分） */
/**
 * 直播秒开订阅：subscribe(seek_live=1) 后立即 seek 到当前 GOP 关键帧。
 *
 * 与裸 subscribe 的区别：保证首个媒体帧是 IDR，播放器无需等下一个关键帧，
 * 首屏更快；代价是丢掉当前 GOP 里 IDR 之前的部分。
 */
ZMS_API zn_err_t zms_media_source_subscribe_gop(zms_media_source *s, zms_media_subscriber *out);
/** 点播：attach vod_buffer reader（from_beginning=1） */
/**
 * 点播订阅：attach vod_buffer reader 并从头读取。
 * @return ZN_OK 成功；源不是点播、或没有 vod_buffer 时返回错误
 */
ZMS_API zn_err_t zms_media_source_subscribe_vod(zms_media_source *s, zms_media_subscriber *out);

/**
 * 统一路径规则：首段为 app，app/ 之后整段为 stream（媒体源 id，可含 /）。
 * @param path_or_url 完整 URL 或路径（可带 / 前缀）
 */
ZMS_API void zms_media_split_path(const char *path_or_url, char *app, char *stream);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CORE_STREAM_HUB_H */
