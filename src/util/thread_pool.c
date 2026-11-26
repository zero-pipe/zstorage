/**
 * @file thread_pool.c
 * @brief VOD / 通用线程池。
 */
#include "zms/util/thread_pool.h"
#include "zn/poller/poller.h"
#include "zn/thread/thread_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>

static zn_thread_pool *g_pool;

void zms_vod_thread_pool_init(int thread_count)
{
    zms_vod_thread_pool_fini();
    if (thread_count <= 0) {
        return;
    }

    {
        zn_thread_pool_opts_t tp_opts;
        tp_opts.thread_count = (unsigned)thread_count;
        tp_opts.priority = ZB_THREAD_PRIO_NORMAL;
        tp_opts.auto_start = 1;
        g_pool = zn_thread_pool_create(&tp_opts);
        if (!g_pool) {
            zn_warn("vod thread_pool create failed, VOD file work runs on poller");
        }
    }
}

void zms_vod_thread_pool_fini(void)
{
    if (g_pool) {
        zn_thread_pool_destroy(g_pool);
        g_pool = NULL;
    }
}

int zms_vod_thread_pool_enabled(void)
{
    return g_pool != NULL;
}

typedef struct zms_vod_thread_pool_run_ctx {
    zn_poller *io;
    zms_vod_thread_pool_work_fn work;
    zms_vod_thread_pool_on_io_fn on_io;
    void *user;
} zms_vod_thread_pool_run_ctx;

static void on_io_tramp(void *arg)
{
    zms_vod_thread_pool_run_ctx *ctx = (zms_vod_thread_pool_run_ctx *)arg;
    if (ctx->on_io) {
        ctx->on_io(ctx->user);
    }
    free(ctx);
}

static void work_tramp(void *arg)
{
    zms_vod_thread_pool_run_ctx *ctx = (zms_vod_thread_pool_run_ctx *)arg;
    if (ctx->work) {
        ctx->work(ctx->user);
    }
    if (ctx->io) {
        /* 投递失败（poller 已停止或已销毁）时不能在 worker 线程执行 on_io：
         * on_io 只允许在 poller 线程访问会话状态。这里仅释放 ctx 并记录，
         * 避免 ctx 泄漏；调用方的 pending 状态需由上层处理。 */
        if (zn_poller_async(ctx->io, on_io_tramp, ctx, 0) != ZN_OK) {
            zn_error("vod thread pool: failed to post completion to poller, dropping");
            free(ctx);
        }
    } else {
        on_io_tramp(ctx);
    }
}

zn_err_t zms_vod_thread_pool_run(zn_poller *io, zms_vod_thread_pool_work_fn work,
                                  zms_vod_thread_pool_on_io_fn on_io, void *user)
{
    zms_vod_thread_pool_run_ctx *ctx;

    if (!work || !on_io) {
        return ZN_ERR_INVALID;
    }

    if (!g_pool) {
        /* 没有 IO 线程池时不能退回调用线程执行：调用方通常是 poller，
         * 这里同步跑 work() 等于在事件循环上做磁盘 IO。直接失败，
         * 由调用方回 503 并打印一次告警。 */
        return ZN_ERR_PLATFORM;
    }

    ctx = (zms_vod_thread_pool_run_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ZN_ERR_NOMEM;
    }
    ctx->io = io;
    ctx->work = work;
    ctx->on_io = on_io;
    ctx->user = user;

    if (zn_thread_pool_async(g_pool, work_tramp, ctx, 0) != ZN_OK) {
        free(ctx);
        return ZN_ERR_PLATFORM;
    }
    return ZN_OK;
}
