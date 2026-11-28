/**
 * @file buf_pool.h
 * @brief 缓冲区池（poller 本地池）。
 */
#ifndef ZCS_UTIL_BUF_POOL_H
#define ZCS_UTIL_BUF_POOL_H

/**
 * ZCS 缓冲池（对齐 [general] bufPoolEnable / bufPoolMode / bufPoolMaxPerBucket）。
 *
 * - global：仅全局池（thread_safe）→ zcs_buf_pool_acquire、zn_buf_alloc（ring）
 * - per_poller：仅各 poller 本地池（无锁；须 zcs_poller_pool_buf_attach）
 * - hybrid：全局(加锁) + per_poller(无锁)（推荐服务器）
 *
 * 会话热路径优先 zcs_buf_pool_slot_*_poller，避免打到全局锁。
 * 跨 poller 共享载荷（GOP ring）用 zn_buf_alloc；连接内发送用 alloc_local。
 * 无锁本地池末次 unref 若不在 owner 线程，znet 会 async 回投再入池。
 * 池内部为每档 freelist；可用 zcs_buf_pool_get_stats 看命中/越档。
 */
#include "zcs/zcs_export.h"
#include "zn/util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_config;
struct zn_poller;
struct zn_poller_pool;

/**
 * 按配置初始化缓冲池：读 bufPoolEnable/Mode，global/hybrid 时建全局加锁池并设为 zn_buf 共享池。
 * 会先 fini 旧池，可重复调用；关闭时 zn_buf_alloc 退化为 malloc。
 */
ZCS_API void zcs_buf_pool_init(const struct zcs_config *cfg);
/** 打印最终统计并销毁全局池、解除 zn_buf 共享池绑定。进程退出时调用。 */
ZCS_API void zcs_buf_pool_fini(void);

/** poller_pool 创建并 start 后调用（per_poller / hybrid） */
ZCS_API void zcs_poller_pool_buf_attach(struct zn_poller_pool *pool, const struct zcs_config *cfg);
/** 解绑并释放该 poller_pool 的本地缓冲池；销毁 poller_pool 前调用。 */
ZCS_API void zcs_poller_pool_buf_detach(struct zn_poller_pool *pool);

/** 查询缓冲池是否启用（bufPoolEnable）；未启用时各 alloc 走 malloc。 */
ZCS_API int zcs_buf_pool_enabled(void);
/** 查询缓冲池模式（global/per_poller/hybrid）。 */
ZCS_API int zcs_buf_pool_mode(void);

/** 全局共享池统计；无全局池时返回 0 并清零 out */
ZCS_API int zcs_buf_pool_get_stats(zn_buf_pool_stats *out);
/** 清零全局共享池统计计数（用于观测窗口重置）。 */
ZCS_API void zcs_buf_pool_reset_stats(void);

/** 裸指针槽位（ingress 工作区等），走全局池 */
ZCS_API void *zcs_buf_pool_acquire(size_t size, size_t *out_cap);
/**
 * 归还 acquire 得到的裸指针。
 * @param cap 必须与 acquire 时的 out_cap 一致，池按档位索引；不匹配会被丢弃。
 */
ZCS_API void zcs_buf_pool_release(void *ptr, size_t cap);

/** 清空槽位：归还 data 并把 *data/*cap 置零；对空槽/空指针安全。 */
ZCS_API void zcs_buf_pool_slot_clear(uint8_t **data, size_t *cap);
/**
 * 保证槽位容量 >= len：已够则原样返回，否则清空后重新 acquire。
 * @return 可用缓冲；len==0 或参数缺失返回 NULL
 */
ZCS_API void *zcs_buf_pool_slot_resize(uint8_t **data, size_t *cap, size_t len);

/** 同 poller 槽位：优先 poller 本地池（RTMP tag 等连接内热路径） */
ZCS_API void zcs_buf_pool_slot_clear_poller(uint8_t **data, size_t *cap, struct zn_poller *poller);
/** poller 版槽位扩容：优先用 poller 本地无锁池，回退全局池。语义同 slot_resize。 */
ZCS_API void *zcs_buf_pool_slot_resize_poller(uint8_t **data, size_t *cap, size_t len,
                                              struct zn_poller *poller);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_UTIL_BUF_POOL_H */
