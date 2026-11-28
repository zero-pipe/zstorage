/**
 * @file buf_pool.c
 * @brief 缓冲区池（poller 本地池）。
 */
#include "zcs/util/buf_pool.h"
#include "zcs/ops/config/config.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static int g_buf_pool_enabled = 1;
static int g_buf_pool_mode = ZCS_BUF_POOL_MODE_HYBRID;
static zn_buf_pool *g_global_pool;

static zn_buf_pool_opts pool_opts_from_cfg(const zcs_config *cfg, int thread_safe)
{
    zn_buf_pool_opts opt;

    memset(&opt, 0, sizeof(opt));
    opt.max_per_bucket = cfg && cfg->general.buf_pool_max_per_bucket > 0
                             ? cfg->general.buf_pool_max_per_bucket
                             : ZCS_BUF_POOL_MAX_PER_BUCKET_DEFAULT;
    opt.thread_safe = thread_safe ? 1 : 0;
    return opt;
}

void zcs_buf_pool_init(const zcs_config *cfg)
{
    zcs_buf_pool_fini();

    g_buf_pool_enabled = cfg ? cfg->general.buf_pool_enable : 1;
    g_buf_pool_mode = cfg ? cfg->general.buf_pool_mode : ZCS_BUF_POOL_MODE_HYBRID;

    if (!g_buf_pool_enabled) {
        zn_buf_set_shared_pool(NULL);
        zn_info("buf pool disabled (bufPoolEnable=0), zn_buf_alloc uses malloc");
        return;
    }

    if (g_buf_pool_mode == ZCS_BUF_POOL_MODE_GLOBAL ||
        g_buf_pool_mode == ZCS_BUF_POOL_MODE_HYBRID) {
        /* 全局池跨 poller / ring，必须加锁 */
        zn_buf_pool_opts opt = pool_opts_from_cfg(cfg, 1);
        g_global_pool = zn_buf_pool_create(&opt);
        zn_buf_set_shared_pool(g_global_pool);
    } else {
        zn_buf_set_shared_pool(NULL);
    }
}

void zcs_buf_pool_fini(void)
{
    if (g_global_pool) {
        zn_buf_pool_stats st;
        zn_buf_pool_get_stats(g_global_pool, &st);
        zn_info("buf pool fini stats: hit=%llu miss=%llu oversize=%llu cached=%llu dropped=%llu",
                 (unsigned long long)st.acquire_hit, (unsigned long long)st.acquire_miss,
                 (unsigned long long)st.acquire_oversize, (unsigned long long)st.release_cached,
                 (unsigned long long)st.release_dropped);
    }
    zn_buf_set_shared_pool(NULL);
    if (g_global_pool) {
        zn_buf_pool_destroy(g_global_pool);
        g_global_pool = NULL;
    }
}

void zcs_poller_pool_buf_attach(zn_poller_pool *pool, const zcs_config *cfg)
{
    if (!pool || !cfg || !cfg->general.buf_pool_enable) {
        return;
    }
    if (cfg->general.buf_pool_mode != ZCS_BUF_POOL_MODE_PER_POLLER &&
        cfg->general.buf_pool_mode != ZCS_BUF_POOL_MODE_HYBRID) {
        return;
    }

    {
        /* poller 本地池：单线程访问，无锁 */
        zn_buf_pool_opts opt = pool_opts_from_cfg(cfg, 0);
        if (zn_poller_pool_attach_buf_pools(pool, &opt) != ZN_OK) {
            zn_warn("poller_pool attach buf pools failed");
        }
    }
}

void zcs_poller_pool_buf_detach(zn_poller_pool *pool)
{
    if (pool) {
        zn_poller_pool_detach_buf_pools(pool);
    }
}

int zcs_buf_pool_enabled(void)
{
    return g_buf_pool_enabled;
}

int zcs_buf_pool_mode(void)
{
    return g_buf_pool_mode;
}

int zcs_buf_pool_get_stats(zn_buf_pool_stats *out)
{
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!g_buf_pool_enabled || !g_global_pool) {
        return 0;
    }
    zn_buf_pool_get_stats(g_global_pool, out);
    return 1;
}

void zcs_buf_pool_reset_stats(void)
{
    if (g_global_pool) {
        zn_buf_pool_reset_stats(g_global_pool);
    }
}

void *zcs_buf_pool_acquire(size_t size, size_t *out_cap)
{
    if (size == 0) {
        return NULL;
    }
    if (g_buf_pool_enabled && g_global_pool) {
        return zn_buf_pool_acquire(g_global_pool, size, out_cap);
    }
    void *p = malloc(size);
    if (p && out_cap) {
        *out_cap = size;
    }
    return p;
}

void zcs_buf_pool_release(void *ptr, size_t cap)
{
    if (!ptr) {
        return;
    }
    if (g_buf_pool_enabled && g_global_pool) {
        zn_buf_pool_release(g_global_pool, ptr, cap);
    } else {
        free(ptr);
    }
    (void)cap;
}

void zcs_buf_pool_slot_clear(uint8_t **data, size_t *cap)
{
    if (!data) {
        return;
    }
    if (*data) {
        zcs_buf_pool_release(*data, cap ? *cap : 0);
        *data = NULL;
    }
    if (cap) {
        *cap = 0;
    }
}

void *zcs_buf_pool_slot_resize(uint8_t **data, size_t *cap, size_t len)
{
    if (!data || !cap || len == 0) {
        return NULL;
    }
    if (*data && *cap >= len) {
        return *data;
    }
    zcs_buf_pool_slot_clear(data, cap);
    *data = (uint8_t *)zcs_buf_pool_acquire(len, cap);
    return *data;
}

static void *acquire_for_poller(zn_poller *poller, size_t size, size_t *out_cap)
{
    zn_buf_pool *local;

    if (size == 0) {
        return NULL;
    }
    local = zn_poller_buf_pool(poller);
    if (g_buf_pool_enabled && local) {
        return zn_buf_pool_acquire(local, size, out_cap);
    }
    return zcs_buf_pool_acquire(size, out_cap);
}

static void release_for_poller(zn_poller *poller, void *ptr, size_t cap)
{
    zn_buf_pool *local;

    if (!ptr) {
        return;
    }
    local = zn_poller_buf_pool(poller);
    if (g_buf_pool_enabled && local) {
        zn_buf_pool_release(local, ptr, cap);
    } else {
        zcs_buf_pool_release(ptr, cap);
    }
}

void zcs_buf_pool_slot_clear_poller(uint8_t **data, size_t *cap, zn_poller *poller)
{
    if (!data) {
        return;
    }
    if (*data) {
        release_for_poller(poller, *data, cap ? *cap : 0);
        *data = NULL;
    }
    if (cap) {
        *cap = 0;
    }
}

void *zcs_buf_pool_slot_resize_poller(uint8_t **data, size_t *cap, size_t len, zn_poller *poller)
{
    if (!data || !cap || len == 0) {
        return NULL;
    }
    if (*data && *cap >= len) {
        return *data;
    }
    zcs_buf_pool_slot_clear_poller(data, cap, poller);
    *data = (uint8_t *)acquire_for_poller(poller, len, cap);
    return *data;
}
