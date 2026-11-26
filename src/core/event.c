/**
 * @file event.c
 * @brief 媒体事件与 tuple 构造（源/端口）。
 */

#include "zms/core/event.h"
#include "zms/core/stream/url.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/media/codec/codec_id.h"
#include "zms/core/stream/vod_source_port.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <stdlib.h>
#include <string.h>

/*
 * 全局媒体事件上下文。
 * 启动时由 zms_media_events_set() / zms_media_events_set_server_ports() 设置一次，
 * 须在任何工作线程启动前完成，故无需互斥锁。
 */
typedef struct zms_media_event_ctx {
    zms_media_events events;
    zn_poller *poller;
    int none_reader_delay_ms;
    int continue_push_ms;
    zms_media_server_ports server_ports;
    int has_server_ports;
} zms_media_event_ctx;

static zms_media_event_ctx g_ctx = {
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL}, NULL, 20000, 15000, {0, 0, 0, 0}, 0};

ZMS_API const char *zms_media_origin_str(zms_media_origin origin)
{
    switch (origin) {
    case ZMS_ORIGIN_RTMP_PUSH:
        return "rtmp_push";
    case ZMS_ORIGIN_RTSP_PUSH:
        return "rtsp_push";
    case ZMS_ORIGIN_SRT_PUSH:
        return "srt_push";
    case ZMS_ORIGIN_RTP_PS_PUSH:
        return "rtp_ps_push";
    case ZMS_ORIGIN_WEBRTC_PUSH:
        return "webrtc_push";
    case ZMS_ORIGIN_PULL:
        return "pull";
    case ZMS_ORIGIN_MP4_VOD:
        return "mp4_vod";
    default:
        return "unknown";
    }
}

void zms_media_events_set_server_ports(const zms_media_server_ports *ports)
{
    if (!ports) {
        return;
    }
    g_ctx.server_ports = *ports;
    g_ctx.has_server_ports = 1;
}

const zms_media_server_ports *zms_media_events_server_ports(void)
{
    return g_ctx.has_server_ports ? &g_ctx.server_ports : NULL;
}

void zms_media_events_set(zn_poller *poller, const zms_media_events *events,
                          int none_reader_delay_ms)
{
    g_ctx.poller = poller;
    if (events) {
        g_ctx.events = *events;
    } else {
        memset(&g_ctx.events, 0, sizeof(g_ctx.events));
    }
    if (none_reader_delay_ms > 0) {
        g_ctx.none_reader_delay_ms = none_reader_delay_ms;
    }
}

zn_poller *zms_media_events_poller(void)
{
    return g_ctx.poller;
}

void zms_media_tuple_from_source(const zms_media_source *src, zms_media_tuple *tuple)
{
    if (!tuple) {
        return;
    }
    memset(tuple, 0, sizeof(*tuple));
    if (!src) {
        return;
    }
    strncpy(tuple->schema, src->schema, sizeof(tuple->schema) - 1);
    strncpy(tuple->vhost, src->vhost, sizeof(tuple->vhost) - 1);
    strncpy(tuple->app, src->app, sizeof(tuple->app) - 1);
    strncpy(tuple->stream, src->stream, sizeof(tuple->stream) - 1);
}

typedef struct zms_media_none_reader_ctx {
    zms_media_tuple tuple;
    zn_poller_timer *timer;
    struct zms_media_none_reader_ctx *next;
} zms_media_none_reader_ctx;

static zms_media_none_reader_ctx *g_none_reader_pending;
static zb_mutex *g_none_reader_mtx;
static int g_media_events_fini;

static void none_reader_pending_init(void)
{
    if (!g_none_reader_mtx) {
        g_none_reader_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    }
}

static void none_reader_pending_add(zms_media_none_reader_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    none_reader_pending_init();
    zb_mutex_lock(g_none_reader_mtx);
    ctx->next = g_none_reader_pending;
    g_none_reader_pending = ctx;
    zb_mutex_unlock(g_none_reader_mtx);
}

/*
 * 从待触发链表里摘除 ctx（poller 线程或 fini 路径调用）。
 * 为什么要在 fire 回调一开始就摘：定时器已到点，若不先摘除，fini 路径可能
 * 在同一 ctx 上重复 cancel/free，与 fire 产生 use-after-free。链表操作加
 * g_none_reader_mtx，因为 add/remove 来自不同线程（推流线程 vs poller）。
 */
static void none_reader_pending_remove(zms_media_none_reader_ctx *ctx)
{
    zms_media_none_reader_ctx **pp;

    if (!ctx || !g_none_reader_mtx) {
        return;
    }
    zb_mutex_lock(g_none_reader_mtx);
    pp = &g_none_reader_pending;
    while (*pp) {
        if (*pp == ctx) {
            *pp = ctx->next;
            break;
        }
        pp = &(*pp)->next;
    }
    zb_mutex_unlock(g_none_reader_mtx);
}

/*
 * none-reader 延时器到点回调（在 g_ctx.poller 线程执行）。
 *
 * 到这里并不等于“确实没人看”：延时窗口内可能又有播放端连上，所以必须
 * 重新查一次 reader_count，>0 就静默丢弃本次判定。这就是“延时 + 复查”
 * 组合的意义——避免客户端短暂重连（如 HLS 分段间隙）被误报成无人观看。
 *
 * 复查后仍为 0 才回调上层并触发 webhook；VOD 源额外主动 stop 释放资源，
 * 因为它不会有推流端重新激活，留着只是占内存。
 */
static uint64_t on_none_reader_fire(void *user)
{
    zms_media_none_reader_ctx *ctx = (zms_media_none_reader_ctx *)user;

    if (!ctx) {
        return 0;
    }
    none_reader_pending_remove(ctx);
    ctx->timer = NULL;

    if (g_media_events_fini || !g_ctx.poller) {
        free(ctx);
        return 0;
    }

    zms_media_source *src =
        zms_media_source_find(ctx->tuple.schema, ctx->tuple.app, ctx->tuple.stream);
    int readers = src ? zms_media_source_reader_count(src) : 0;
    if (readers > 0) {
        free(ctx);
        return 0;
    }

    zn_debug("[event] none_reader app=%s stream=%s", ctx->tuple.app, ctx->tuple.stream);
    if (g_ctx.events.on_media_none_reader) {
        g_ctx.events.on_media_none_reader(&ctx->tuple, g_ctx.events.user);
    }
    zms_webhook_on_none_reader(&ctx->tuple);
    if (src && zms_media_source_is_vod(src)) {
        zms_vod_source_stop(src);
    }
    free(ctx);
    return 0;
}

static void schedule_none_reader_check(const zms_media_tuple *tuple)
{
    zms_media_none_reader_ctx *ctx;

    if (g_media_events_fini || !g_ctx.poller || g_ctx.none_reader_delay_ms <= 0 || !tuple) {
        return;
    }
    {
        /* 早退：既没有注册回调、也没配 webhook、又不是 VOD，则本次判定
         * 没有任何副作用，不必白占一个定时器槽。 */
        zms_media_source *src = zms_media_source_find(tuple->schema, tuple->app, tuple->stream);
        int is_vod = src && zms_media_source_is_vod(src);
        if (!is_vod && !g_ctx.events.on_media_none_reader &&
            !zms_webhook_none_reader_configured()) {
            return;
        }
    }
    ctx = (zms_media_none_reader_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->tuple = *tuple;
    ctx->timer = zn_poller_do_delay(g_ctx.poller, (uint64_t)g_ctx.none_reader_delay_ms,
                                     on_none_reader_fire, ctx);
    if (!ctx->timer) {
        free(ctx);
        return;
    }
    none_reader_pending_add(ctx);
}

/*
 * 观众数变化通知。仅在计数降到 0 时才安排 none-reader 延时检查；
 * 回升（>0）不取消已排队的检查——交给 fire 时复查去兜底，这样能省掉
 * “回溯待触发链表并 cancel 定时器”的复杂逻辑，代价只是偶尔一次空转。
 */
static void notify_reader_changed(zms_media_source *src)
{
    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    int count = zms_media_source_reader_count(src);
    if (g_ctx.events.on_media_reader_changed) {
        g_ctx.events.on_media_reader_changed(&tuple, count, g_ctx.events.user);
    }

    if (count > 0) {
        return;
    }
    schedule_none_reader_check(&tuple);
}


void zms_media_events_set_continue_push_ms(int ms)
{
    g_ctx.continue_push_ms = ms > 0 ? ms : 0;
}

int zms_media_continue_push_ms(void)
{
    return g_ctx.continue_push_ms;
}

typedef struct zms_media_linger_ctx {
    zms_media_tuple tuple;
    zms_media_origin origin;
    zn_poller_timer *timer;
    struct zms_media_linger_ctx *next;
} zms_media_linger_ctx;

static zms_media_linger_ctx *g_linger_pending;
static zb_mutex *g_linger_mtx;

static void linger_pending_init(void)
{
    if (!g_linger_mtx) {
        g_linger_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    }
}

static void linger_pending_remove(zms_media_linger_ctx *ctx)
{
    zms_media_linger_ctx **pp;

    if (!ctx || !g_linger_mtx) {
        return;
    }
    zb_mutex_lock(g_linger_mtx);
    for (pp = &g_linger_pending; *pp; pp = &(*pp)->next) {
        if (*pp == ctx) {
            *pp = ctx->next;
            break;
        }
    }
    zb_mutex_unlock(g_linger_mtx);
}

/*
 * linger 延时器到点回调（poller 线程）。
 *
 * linger 的用途：推流端断开后不立刻销毁源，先保留 GOP/轨道 continue_push_ms，
 * 让正在看的播放端不至于瞬间断流，也给推流端快速重连留出复用同一源的窗口。
 *
 * 到点时仍要复核 !src->publishing：窗口内推流端可能已经重连并重新置位
 * publishing=1，此时绝不能清源，否则会把刚恢复的直播又拆掉。
 */
static uint64_t on_linger_fire(void *user)
{
    zms_media_linger_ctx *ctx = (zms_media_linger_ctx *)user;
    zms_media_source *src;

    if (!ctx) {
        return 0;
    }
    linger_pending_remove(ctx);
    ctx->timer = NULL;
    if (!g_media_events_fini && g_ctx.poller) {
        src = zms_media_source_find(ctx->tuple.schema, ctx->tuple.app, ctx->tuple.stream);
        if (src && !src->publishing) {
            zn_info("[event] linger expired app=%s stream=%s", ctx->tuple.app,
                     ctx->tuple.stream);
            zms_media_event_publish_fini(src, ctx->origin);
            zms_media_source_clear(src);
        }
    }
    free(ctx);
    return 0;
}

void zms_media_linger_cancel(const char *app, const char *stream)
{
    zms_media_linger_ctx *ctx;
    zms_media_linger_ctx *hit = NULL;

    if (!app || !stream || !g_linger_mtx) {
        return;
    }
    zb_mutex_lock(g_linger_mtx);
    for (ctx = g_linger_pending; ctx; ctx = ctx->next) {
        if (strcmp(ctx->tuple.app, app) == 0 && strcmp(ctx->tuple.stream, stream) == 0) {
            hit = ctx;
            break;
        }
    }
    zb_mutex_unlock(g_linger_mtx);
    if (hit) {
        linger_pending_remove(hit);
        if (hit->timer) {
            zn_poller_timer_cancel(hit->timer);
        }
        free(hit);
    }
}

/*
 * 进入 linger 状态：保留源（GOP/轨道）但标记为“不再推流”。
 *
 * 返回 1 表示已排定延时销毁，0 表示未进入 linger（无 continue_push_ms 配置、
 * 无 poller、无 GOP 缓存或本身就是 VOD）。VOD 排除在外是因为它由点播本身
 * 的生命周期管理，不走“推流断开”语义。
 *
 * 注意 publishing=0 与 mp4_recorder_stop 的位置：录像必须在断流时立即收尾
 * （mp4 文件需要写 moov 尾部），不能跟着 linger 一起延迟，否则录像文件会
 * 一直缺索引、客户端读不了。
 */
int zms_media_source_linger(zms_media_source *src, zms_media_origin origin)
{
    zms_media_linger_ctx *ctx;

    if (!src || g_ctx.continue_push_ms <= 0 || !g_ctx.poller || !src->gop_queue) {
        return 0;
    }
    if (zms_media_source_is_vod(src)) {
        return 0;
    }
    ctx = (zms_media_linger_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    zms_media_tuple_from_source(src, &ctx->tuple);
    ctx->origin = origin;
    linger_pending_init();
    if (!g_linger_mtx) {
        free(ctx);
        return 0;
    }
    ctx->timer = zn_poller_do_delay(g_ctx.poller, (uint64_t)g_ctx.continue_push_ms,
                                     on_linger_fire, ctx);
    if (!ctx->timer) {
        free(ctx);
        return 0;
    }
    zb_mutex_lock(g_linger_mtx);
    ctx->next = g_linger_pending;
    g_linger_pending = ctx;
    zb_mutex_unlock(g_linger_mtx);
    zms_media_source_registry_lock();
    src->publishing = 0; /* 保留 GOP/轨道，但不再视为在推 */
    zms_media_source_registry_unlock();
    if (src->mp4_recorder_stop) {
        src->mp4_recorder_stop(src); /* 录像在断流时收尾，不跟随 linger 延迟 */
    }
    if (src->zs_recorder_stop) {
        src->zs_recorder_stop(src); /* 云存储同样在断流时封口 */
    }
    zn_info("[event] linger app=%s stream=%s ms=%d origin=%s", ctx->tuple.app, ctx->tuple.stream,
             g_ctx.continue_push_ms, zms_media_origin_str(origin));
    return 1;
}

void zms_media_event_publish(zms_media_source *src, zms_media_origin origin)
{
    if (!src) {
        return;
    }
    zms_media_source_registry_lock();
    src->publish_origin = (int)origin;
    src->publishing = 1;
    zms_media_source_registry_unlock();
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    zn_info("[event] publish app=%s stream=%s origin=%s video=%s %dx%d audio=%s %dHz", tuple.app,
             tuple.stream, zms_media_origin_str(origin),
             src->has_video ? zms_codec_name(src->video.codec) : "-",
             src->has_video ? (int)src->video.width : 0,
             src->has_video ? (int)src->video.height : 0,
             src->has_audio ? zms_codec_name(src->audio.codec) : "-",
             src->has_audio ? (int)src->audio.sample_rate : 0);
    if (g_ctx.has_server_ports) {
        zms_media_urls_log_publish(src, &g_ctx.server_ports);
    }
    if (g_ctx.events.on_media_publish) {
        g_ctx.events.on_media_publish(&tuple, origin, g_ctx.events.user);
    }
    zms_webhook_on_stream_register(src, origin);
}

void zms_media_event_publish_fini(zms_media_source *src, zms_media_origin origin)
{
    if (!src) {
        return;
    }
    /* 先停录像（写 moov 收尾），再判断 publishing：即使本函数被重复调用、
     * 或源从未进入 publishing，录像也应被正确关闭。publishing 已为 0 时
     * 提前返回，避免向 webhook 重复发 unregister。 */
    if (src->mp4_recorder_stop) {
        src->mp4_recorder_stop(src);
    }
    if (src->zs_recorder_stop) {
        src->zs_recorder_stop(src);
    }
    zms_media_source_registry_lock();
    if (!src->publishing) {
        zms_media_source_registry_unlock();
        return;
    }
    src->publishing = 0;
    zms_media_source_registry_unlock();
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    zn_debug("[event] publish_fini app=%s stream=%s origin=%s", tuple.app, tuple.stream,
              zms_media_origin_str(origin));
    if (g_ctx.events.on_media_publish_fini) {
        g_ctx.events.on_media_publish_fini(&tuple, origin, g_ctx.events.user);
    }
    zms_webhook_on_stream_unregister(src, origin);
}

void zms_media_event_play(zms_media_source *src, const char *player_schema)
{
    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    zn_debug("[event] play app=%s stream=%s player=%s", tuple.app, tuple.stream,
              player_schema ? player_schema : "?");
    if (g_ctx.events.on_media_play) {
        g_ctx.events.on_media_play(&tuple, player_schema, g_ctx.events.user);
    }
}

void zms_media_event_stop(zms_media_source *src, const char *player_schema, uint64_t play_start_ms)
{
    uint64_t duration_ms;

    if (!src) {
        return;
    }
    zms_media_tuple tuple;
    zms_media_tuple_from_source(src, &tuple);
    zn_debug("[event] stop app=%s stream=%s player=%s", tuple.app, tuple.stream,
              player_schema ? player_schema : "?");
    if (g_ctx.events.on_media_stop) {
        g_ctx.events.on_media_stop(&tuple, player_schema, g_ctx.events.user);
    }
    duration_ms = (play_start_ms > 0) ? (zn_monotonic_ms() - play_start_ms) : 0;
    zms_webhook_on_play_stop(&tuple, player_schema, duration_ms);
}

void zms_media_source_reader_add(zms_media_source *src)
{
    if (!src) {
        return;
    }
    /* 计数在 registry 锁内维护，通知在锁外做：回调/webhook 可能阻塞或反查
     * 其它源，若持锁调用极易死锁。 */
    zms_media_source_registry_lock();
    ++src->reader_count;
    zms_media_source_registry_unlock();
    notify_reader_changed(src);
}

void zms_media_source_reader_remove(zms_media_source *src)
{
    if (!src) {
        return;
    }
    /* 防御下溢：重复 remove 不应把计数变负，否则“无人观看”判定失真。 */
    zms_media_source_registry_lock();
    if (src->reader_count <= 0) {
        zms_media_source_registry_unlock();
        return;
    }
    --src->reader_count;
    zms_media_source_registry_unlock();
    notify_reader_changed(src);
}

int zms_media_source_reader_count(const zms_media_source *src)
{
    if (!src) {
        return 0;
    }
    zms_media_source_registry_lock();
    int n = src->reader_count;
    zms_media_source_registry_unlock();
    return n;
}

/*
 * 进程级清理：置 fini 标志后，所有到点回调都只看标志、不再触达业务，
 * 从而与正在排空的定时器安全竞争。取链表头后解锁再逐个 cancel/free，
 * 避免在持锁期间调用 poller 的 cancel（可能与 poller 自身锁序冲突）。
 */
void zms_media_events_fini(void)
{
    zms_media_none_reader_ctx *pending;
    zms_media_none_reader_ctx *next;

    g_media_events_fini = 1;
    g_ctx.poller = NULL;
    memset(&g_ctx.events, 0, sizeof(g_ctx.events));

    none_reader_pending_init();
    zb_mutex_lock(g_none_reader_mtx);
    pending = g_none_reader_pending;
    g_none_reader_pending = NULL;
    zb_mutex_unlock(g_none_reader_mtx);

    while (pending) {
        next = pending->next;
        if (pending->timer) {
            zn_poller_timer_cancel(pending->timer);
        }
        free(pending);
        pending = next;
    }

    linger_pending_init();
    zb_mutex_lock(g_linger_mtx);
    {
        zms_media_linger_ctx *lc = g_linger_pending;

        g_linger_pending = NULL;
        zb_mutex_unlock(g_linger_mtx);
        while (lc) {
            zms_media_linger_ctx *ln = lc->next;

            if (lc->timer) {
                zn_poller_timer_cancel(lc->timer);
            }
            free(lc);
            lc = ln;
        }
    }
}
