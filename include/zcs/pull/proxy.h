#ifndef ZCS_PULL_PROXY_H
#define ZCS_PULL_PROXY_H

/**
 * @file proxy.h
 * @brief 拉流代理：将远端 RTSP/RTMP/HTTP-FLV/HLS 接入本地 media_source。
 *
 * 创建绑定已注册源的 @ref zcs_live_ingest，观众可通过本地服务器的
 * RTSP、RTMP、HTTP-FLV、HLS 播放。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/core/stream/hub.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;
struct zn_poller_pool;
typedef struct zn_poller_pool zn_poller_pool;

typedef struct zcs_live_pull_proxy zcs_live_pull_proxy;

typedef void (*zcs_live_pull_proxy_on_ready_cb)(void *user);
typedef void (*zcs_live_pull_proxy_on_error_cb)(zn_err_t err, void *user);

typedef struct zcs_live_pull_proxy_opts {
    /** poller 与 poller_pool 须恰好设置其一。 */
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    /** 拉流 URL：rtsp(s)://、rtmp(s)://、http(s)://…flv、http(s)://…m3u8 */
    const char *pull_url;
    /** rtsps/rtmps 且未用默认 SSL 上下文时必填。 */
    struct zn_ssl_ctx *ssl_ctx;
    /** 本地 vhost；NULL/空则为默认 vhost。 */
    const char *vhost;
    /** 本地 app；NULL 或空默认为 "live"。 */
    const char *app;
    /**
     * 本地 stream 名；NULL/空/auto 从拉流 URL 路径推导尾段
     *（如 live/test_265 → test_265）。
     */
    const char *stream;
    /** 可选前缀，加在推导出的 stream 尾段前。 */
    const char *proxy_prefix;
    /** RTSP 拉流 RTP 模式（RTMP 拉流忽略）。 */
    zcs_rtsp_rtp_mode rtp_mode;
    int retry_count;
    int reconnect_delay_ms;
    zcs_live_pull_proxy_on_ready_cb on_ready;
    zcs_live_pull_proxy_on_error_cb on_error;
    void *user;
} zcs_live_pull_proxy_opts;

/**
 * 创建拉流代理（注册到全局表，但尚未开始拉流）。
 * pull_url 与 poller/poller_pool 二选一必须有效。
 * @return 新代理；参数非法或内存不足返回 NULL。
 */
ZCS_API zcs_live_pull_proxy *zcs_live_pull_proxy_create(const zcs_live_pull_proxy_opts *opts);
/** 停止并销毁代理：从全局表摘除、取消重连、断开拉流并释放资源。 */
ZCS_API void zcs_live_pull_proxy_destroy(zcs_live_pull_proxy *p);
/** 开始（或继续）拉流。已 start 时重复调用返回 ZN_OK，不做重复动作。 */
ZCS_API zn_err_t zcs_live_pull_proxy_start(zcs_live_pull_proxy *p);
/** 停止拉流但保留代理对象（可再次 start）；内部会清推流、取消重连。 */
ZCS_API void zcs_live_pull_proxy_stop(zcs_live_pull_proxy *p);
/** 取代理绑定的本地媒体源；@return 借用指针，未就绪返回 NULL。 */
ZCS_API zcs_media_source *zcs_live_pull_proxy_source(zcs_live_pull_proxy *p);
/** 取底层 live_ingest；@return 借用指针，p 为 NULL 时返回 NULL。 */
ZCS_API zcs_live_ingest *zcs_live_pull_proxy_ingress(zcs_live_pull_proxy *p);

/** 构造 Web API key：vhost/app/stream（vhost 空则用 __defaultVhost__）。 */
ZCS_API void zcs_live_pull_proxy_make_key(const char *vhost, const char *app, const char *stream,
                                          char *key, size_t key_cap);

/** 按 Web API key 查找代理；@return 借用指针（不转移所有权），未找到 NULL。 */
ZCS_API zcs_live_pull_proxy *zcs_live_pull_proxy_find_by_key(const char *key);

typedef int (*zcs_live_pull_proxy_visit_cb)(const char *key, zcs_live_pull_proxy *p, void *user);
/**
 * 遍历所有代理（持注册表锁）。
 * @param cb 回调；返回非 0 提前终止
 * @return 已访问的代理数
 * 注意：回调在持锁状态运行，回调内不得再调用代理注册表 API（会自锁）。
 */
ZCS_API int zcs_live_pull_proxy_foreach(zcs_live_pull_proxy_visit_cb cb, void *user);

/** 首段之后的路径尾：live/test_265 → test_265。 */
ZCS_API int zcs_live_pull_proxy_path_tail(const char *pull_url, char *tail, size_t tail_cap);

/** 本地 stream = {prefix/}{tail}；空前缀仅用 tail。 */
ZCS_API int zcs_live_pull_proxy_build_stream(const char *pull_url, const char *prefix, char *stream,
                                             size_t stream_cap);

/** 取代理本地 app；p 为 NULL 时返回空串 ""。 */
ZCS_API const char *zcs_live_pull_proxy_app(const zcs_live_pull_proxy *p);
/** 取代理本地 stream；p 为 NULL 时返回空串 ""。 */
ZCS_API const char *zcs_live_pull_proxy_stream(const zcs_live_pull_proxy *p);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PULL_PROXY_H */
