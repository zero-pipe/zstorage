/**
 * @file zs_config.c
 * @brief [storage] 配置 -> 存储层参数。
 */
#include "storage/zs_config.h"

#include <string.h>

void zs_pool_opts_from_config(zs_pool_opts *out, const zcs_storage_config *cfg)
{
    zs_pool_opts def;

    if (!out) {
        return;
    }
    zs_pool_opts_default(&def);
    if (!cfg) {
        *out = def;
        return;
    }
    memset(out, 0, sizeof(*out));
    out->dir = cfg->dir[0] ? cfg->dir : NULL;
    out->capacity_bytes = cfg->capacity_gb > 0 ? (uint64_t)cfg->capacity_gb * ZS_GB
                                               : def.capacity_bytes;
    out->retention_days = cfg->retention_days > 0 ? (uint32_t)cfg->retention_days
                                                  : def.retention_days;
    out->slot_bytes = cfg->slot_size_mb > 0 ? (uint64_t)cfg->slot_size_mb * ZS_MB
                                            : def.slot_bytes;
    out->index_bytes = cfg->index_kb > 0 ? (uint32_t)((uint64_t)cfg->index_kb * ZS_KB)
                                         : def.index_bytes;
    out->free_watermark_pct = cfg->free_watermark_pct > 0
                                  ? (uint32_t)cfg->free_watermark_pct
                                  : def.free_watermark_pct;
}

void zs_writer_opts_from_config(zs_writer_opts *out, const zcs_storage_config *cfg, zs_pool *pool,
                                uint32_t channel_id, const char *stream)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->pool = pool;
    out->channel_id = channel_id;
    out->stream = stream;
    if (!cfg) {
        return; /* 全 0 让 writer 用内置默认 */
    }
    out->flush_bytes = cfg->flush_kb > 0 ? (uint32_t)((uint64_t)cfg->flush_kb * ZS_KB) : 0u;
    out->flush_ms = cfg->flush_ms > 0 ? (uint32_t)cfg->flush_ms : 0u;
    out->slot_max_ms = cfg->slot_max_second > 0 ? (uint32_t)cfg->slot_max_second * 1000u : 0u;
    out->slot_seal_margin = cfg->seal_margin_mb > 0
                                ? (uint64_t)cfg->seal_margin_mb * ZS_MB
                                : 0ULL;
}
