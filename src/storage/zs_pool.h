/**
 * @file zs_pool.h
 * @brief 槽位池：按容量与保留天数管理一批预分配槽位文件。
 *
 * 生命周期：FREE（可复用）-> WRITING（某通道正在写）-> SEALED（可回放）
 *           -> 超期或容量不足时 reset 回 FREE，文件不删除、不 unlink。
 *
 * 容量模型：槽位总数 = capacity_bytes / slot_bytes（上限 ZS_POOL_MAX_SLOTS）。
 * 盘空间是"一次预分配、循环使用"，所以删除旧录像不会释放磁盘，回收只是
 * 把槽位重置回 FREE。保留天数与容量两条约束同时生效，先到者回收。
 *
 * 启动时扫描目录：状态为 WRITING 的槽位说明上次进程是异常退出，先做崩溃
 * 恢复再封口，不再继续追加（避免跨进程时间线错乱）。
 */
#ifndef ZMS_SRC_STORAGE_ZS_POOL_H
#define ZMS_SRC_STORAGE_ZS_POOL_H

#include "storage/zs_slot.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZS_POOL_MAX_SLOTS (64u * 1024u)
#define ZS_POOL_PATH_MAX 512u

typedef struct zs_pool zs_pool;

typedef struct zs_pool_opts {
    const char *dir;              /**< 槽位目录（需已存在，不存在会尝试创建） */
    uint64_t capacity_bytes;      /**< 存储池容量，0 取默认 2TB */
    uint32_t retention_days;      /**< 保留天数，0 取默认 7 */
    uint64_t slot_bytes;          /**< 单槽文件大小，0 取默认 256MB */
    uint32_t index_bytes;         /**< 槽位索引区大小，0 取默认 1MB */
    uint32_t free_watermark_pct;  /**< 空闲槽位低于该百分比时提前回收，0 取默认 5% */
} zs_pool_opts;

/** 默认值：2TB / 7 天 / 256MB 槽位 / 1MB 索引 / 5% 水位。 */
void zs_pool_opts_default(zs_pool_opts *opts);

typedef struct zs_pool_info {
    uint32_t id;         /**< 槽位全局序号（文件名中的序号） */
    uint32_t channel_id; /**< 所属通道；FREE 时为 0 */
    uint32_t state;
    uint32_t ref_count; /**< 回放钉住计数，>0 时不可回收 */
    uint64_t seg_start_ms;
    uint64_t seg_end_ms;
    char stream[ZS_SLOT_STREAM_MAX];
} zs_pool_info;

typedef struct zs_pool_lease {
    uint32_t id;
    zs_slot *slot;
} zs_pool_lease;

zs_pool *zs_pool_open(const zs_pool_opts *opts);
void zs_pool_close(zs_pool *p);

/**
 * 申请一个可写槽位：优先 FREE，其次新建，最后回收最旧的 SEALED（ref_count==0）。
 * @param channel_id 数字标签（可 0）
 * @param stream     流标识（写入槽位头，回放按它检索）
 * @return 0 成功；-1 无可用槽位/IO 错误
 */
int zs_pool_acquire(zs_pool *p, uint32_t channel_id, const char *stream, uint64_t now_ms,
                    zs_pool_lease *out);

/** 归还槽位：有数据则封口为 SEALED，空槽位直接回 FREE。 */
int zs_pool_release(zs_pool *p, zs_pool_lease *lease);

/** 把正在写的槽位元数据（段起止时间）同步进池，便于回放侧发现。 */
int zs_pool_sync(zs_pool *p, zs_pool_lease *lease);

/** 回放侧钉住/解除钉住，防止被容量回收。 */
int zs_pool_pin(zs_pool *p, uint32_t id);
int zs_pool_unpin(zs_pool *p, uint32_t id);

/** 回收超过保留期的封口槽位，返回回收数量。 */
uint32_t zs_pool_expire(zs_pool *p, uint64_t now_ms);

uint32_t zs_pool_count(const zs_pool *p);
uint32_t zs_pool_free_count(const zs_pool *p);
uint32_t zs_pool_max_slots(const zs_pool *p);
uint32_t zs_pool_channel_count(const zs_pool *p, uint32_t channel_id);
uint64_t zs_pool_used_bytes(const zs_pool *p);
uint64_t zs_pool_capacity_bytes(const zs_pool *p);

int zs_pool_get_info(const zs_pool *p, uint32_t idx, zs_pool_info *out);
int zs_pool_get_info_by_id(const zs_pool *p, uint32_t id, zs_pool_info *out);

/**
 * 按时间范围查找有重叠的槽位，按 seg_start 升序写入 ids。
 * @return 写入的条数（可能被 max 截断）
 */
uint32_t zs_pool_find_range(const zs_pool *p, uint64_t from_ms, uint64_t to_ms, uint32_t *ids,
                            uint32_t max);

/** 同上，但只返回指定流的槽位；stream 为 NULL/空串表示不筛选。 */
uint32_t zs_pool_find_range_stream(const zs_pool *p, const char *stream, uint64_t from_ms,
                                   uint64_t to_ms, uint32_t *ids, uint32_t max);

/** 打开槽位用于回放（调用方负责 zs_slot_close）。 */
zs_slot *zs_pool_open_slot(zs_pool *p, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_STORAGE_ZS_POOL_H */
