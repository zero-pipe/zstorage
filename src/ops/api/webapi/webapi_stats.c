/**
 * @file webapi_stats.c
 * @brief Web API 运行状态：健康检查、getStatistic、缓冲池与进程指标。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zcs/version.h"
#include "zcs/core/stream/hub.h"
#include "zcs/ops/api/json/media_json.h"
#include "zn/platform.h"
#include "zcs/util/buf_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

static uint64_t g_webapi_boot_ms;

void zcs_web_api_note_boot(void)
{
    if (g_webapi_boot_ms == 0) {
        g_webapi_boot_ms = zn_monotonic_ms();
    }
}

uint64_t webapi_uptime_sec(void)
{
    uint64_t now;

    if (g_webapi_boot_ms == 0) {
        return 0;
    }
    now = zn_monotonic_ms();
    return now > g_webapi_boot_ms ? (now - g_webapi_boot_ms) / 1000ull : 0;
}
double webapi_buf_pool_hit_rate(const zn_buf_pool_stats *st)
{
    uint64_t denom;

    if (!st) {
        return 0.0;
    }
    denom = st->acquire_hit + st->acquire_miss;
    if (denom == 0) {
        return 0.0;
    }
    return (double)st->acquire_hit * 100.0 / (double)denom;
}
typedef struct zcs_webapi_health_ctx {
    int media_source;
    int total_reader;
    size_t max_gop_queue_lag;
    size_t total_pending;
    uint64_t total_dropped;
} zcs_webapi_health_ctx;

static int health_visit(zcs_media_source *src, void *user)
{
    zcs_webapi_health_ctx *ctx = (zcs_webapi_health_ctx *)user;
    zcs_media_stats_view st;

    if (!zcs_media_source_is_online(src)) {
        return 0;
    }
    ++ctx->media_source;
    ctx->total_reader += src->reader_count;
    zcs_media_stats_fill(src, src->gop_queue, &st);
    ctx->total_pending += st.gop_queue_pending;
    ctx->total_dropped += st.dropped_frames;
    if (st.gop_queue_max_lag > ctx->max_gop_queue_lag) {
        ctx->max_gop_queue_lag = st.gop_queue_max_lag;
    }
    return 0;
}

/** 进程健康：无需 secret（负载均衡探活）；含环滞后与全局池摘要 */
/* 进程级运维指标：RSS 与打开 fd 数（Linux /proc；其它平台返回 0）。 */
uint64_t webapi_proc_rss_bytes(void)
{
#if defined(__linux__)
    FILE *fp = fopen("/proc/self/statm", "r");
    long pages = 0;
    long total = 0;
    long page_size;

    if (!fp) {
        return 0;
    }
    if (fscanf(fp, "%ld %ld", &total, &pages) != 2) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    (void)total;
    page_size = sysconf(_SC_PAGESIZE);
    return (uint64_t)pages * (uint64_t)page_size;
#else
    return 0;
#endif
}

uint64_t webapi_proc_fd_count(void)
{
#if defined(__linux__)
    DIR *d = opendir("/proc/self/fd");
    struct dirent *ent;
    uint64_t n = 0;

    if (!d) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] != '.') {
            ++n;
        }
    }
    closedir(d);
    return n;
#else
    return 0;
#endif
}

size_t webapi_handle_health(char *body, size_t body_cap, int *http_status)
{
    zcs_webapi_health_ctx ctx;
    zn_buf_pool_stats st;
    int have_pool;

    memset(&ctx, 0, sizeof(ctx));
    memset(&st, 0, sizeof(st));
    zcs_media_source_foreach(health_visit, &ctx, NULL);
    have_pool = zcs_buf_pool_get_stats(&st);
    *http_status = 200;
    return (size_t)snprintf(
        body, body_cap,
        "{\"code\":0,\"data\":{"
        "\"status\":\"ok\","
        "\"version\":\"%s\","
        "\"uptimeSec\":%llu,"
        "\"MediaSource\":%d,"
        "\"totalReaderCount\":%d,"
        "\"totalFrameRingPending\":%zu,"
        "\"maxFrameRingLag\":%zu,"
        "\"totalDroppedFrames\":%llu,"
        "\"rssBytes\":%llu,"
        "\"openFds\":%llu,"
        "\"bufPool\":{"
        "\"enabled\":%s,"
        "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
        "\"cached\":%llu,\"dropped\":%llu,"
        "\"hitRate\":%.2f"
        "}}}",
        ZCS_VERSION_STRING, (unsigned long long)webapi_uptime_sec(), ctx.media_source,
        ctx.total_reader, ctx.total_pending, ctx.max_gop_queue_lag,
        (unsigned long long)ctx.total_dropped, (unsigned long long)webapi_proc_rss_bytes(),
        (unsigned long long)webapi_proc_fd_count(), have_pool ? "true" : "false",
        (unsigned long long)st.acquire_hit, (unsigned long long)st.acquire_miss,
        (unsigned long long)st.acquire_oversize, (unsigned long long)st.release_cached,
        (unsigned long long)st.release_dropped, webapi_buf_pool_hit_rate(&st));
}

size_t webapi_handle_buf_pool_stats(char *body, size_t body_cap, int *http_status)
{
    zn_buf_pool_stats st;
    int have_pool;

    memset(&st, 0, sizeof(st));
    have_pool = zcs_buf_pool_get_stats(&st);
    *http_status = 200;
    if (!have_pool) {
        return (size_t)snprintf(body, body_cap,
                                "{\"code\":0,\"data\":{\"enabled\":false,"
                                "\"note\":\"global pool off or not hybrid/global mode\"}}");
    }
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"data\":{"
                            "\"enabled\":true,"
                            "\"mode\":%d,"
                            "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
                            "\"cached\":%llu,\"dropped\":%llu,"
                            "\"hitRate\":%.2f"
                            "}}",
                            zcs_buf_pool_mode(), (unsigned long long)st.acquire_hit,
                            (unsigned long long)st.acquire_miss,
                            (unsigned long long)st.acquire_oversize,
                            (unsigned long long)st.release_cached,
                            (unsigned long long)st.release_dropped, webapi_buf_pool_hit_rate(&st));
}
typedef struct zcs_webapi_stat_ctx {
    int media_source;
    int total_reader;
    uint64_t total_ingress_bytes;
    uint64_t total_egress_bytes;
    int64_t total_bytes_speed;
    int64_t total_egress_speed;
    size_t max_gop_queue_lag;
    size_t total_pending;
    size_t max_pending;
    size_t total_gop_count;
    uint64_t total_dropped;
} zcs_webapi_stat_ctx;

static int stat_visit(zcs_media_source *src, void *user)
{
    zcs_webapi_stat_ctx *ctx = (zcs_webapi_stat_ctx *)user;
    zcs_media_stats_view st;

    if (!zcs_media_source_is_online(src)) {
        return 0;
    }
    ++ctx->media_source;
    ctx->total_reader += src->reader_count;
    zcs_media_stats_fill(src, src->gop_queue, &st);
    ctx->total_ingress_bytes += st.ingress_bytes;
    ctx->total_egress_bytes += st.egress_bytes;
    ctx->total_bytes_speed += st.bytes_speed;
    ctx->total_egress_speed += st.egress_speed;
    ctx->total_pending += st.gop_queue_pending;
    ctx->total_gop_count += st.gop_queue_gop_count;
    ctx->total_dropped += st.dropped_frames;
    if (st.gop_queue_max_lag > ctx->max_gop_queue_lag) {
        ctx->max_gop_queue_lag = st.gop_queue_max_lag;
    }
    if (st.gop_queue_pending > ctx->max_pending) {
        ctx->max_pending = st.gop_queue_pending;
    }
    return 0;
}
size_t webapi_handle_statistic(char *body, size_t body_cap, int *http_status)
{
    zcs_webapi_stat_ctx ctx;
    zn_buf_pool_stats pst;
    int have_pool;

    memset(&ctx, 0, sizeof(ctx));
    memset(&pst, 0, sizeof(pst));
    zcs_media_source_foreach(stat_visit, &ctx, NULL);
    have_pool = zcs_buf_pool_get_stats(&pst);
    *http_status = 200;
    return (size_t)snprintf(
        body, body_cap,
        "{\"code\":0,\"data\":{"
        "\"uptimeSec\":%llu,"
        "\"MediaSource\":%d,"
        "\"totalReaderCount\":%d,"
        "\"totalBytes\":%llu,"
        "\"bytesSpeed\":%lld,"
        "\"totalEgressBytes\":%llu,"
        "\"egressSpeed\":%lld,"
        "\"totalFrameRingPending\":%zu,"
        "\"maxFrameRingPending\":%zu,"
        "\"maxFrameRingLag\":%zu,"
        "\"totalGopCount\":%zu,"
        "\"totalDroppedFrames\":%llu,"
        "\"rssBytes\":%llu,"
        "\"openFds\":%llu,"
        "\"bufPool\":{"
        "\"enabled\":%s,"
        "\"hit\":%llu,\"miss\":%llu,\"oversize\":%llu,"
        "\"cached\":%llu,\"dropped\":%llu,"
        "\"hitRate\":%.2f"
        "}}}",
        (unsigned long long)webapi_uptime_sec(), ctx.media_source, ctx.total_reader,
        (unsigned long long)ctx.total_ingress_bytes, (long long)ctx.total_bytes_speed,
        (unsigned long long)ctx.total_egress_bytes, (long long)ctx.total_egress_speed,
        ctx.total_pending, ctx.max_pending, ctx.max_gop_queue_lag, ctx.total_gop_count,
        (unsigned long long)ctx.total_dropped, (unsigned long long)webapi_proc_rss_bytes(),
        (unsigned long long)webapi_proc_fd_count(), have_pool ? "true" : "false",
        (unsigned long long)pst.acquire_hit, (unsigned long long)pst.acquire_miss,
        (unsigned long long)pst.acquire_oversize, (unsigned long long)pst.release_cached,
        (unsigned long long)pst.release_dropped, webapi_buf_pool_hit_rate(&pst));
}