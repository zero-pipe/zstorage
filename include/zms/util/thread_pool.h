/**
 * @file thread_pool.h
 * @brief 线程池。
 */
#ifndef ZMS_UTIL_THREAD_POOL_H
#define ZMS_UTIL_THREAD_POOL_H

/**
 * VOD 磁盘阻塞任务：zn_thread_pool 执行 work，完成后 zn_poller_async 回 I/O 线程。
 * 会话/点播事件循环用 poller_pool；VOD 文件操作用 thread_pool。
 *
 * init：zms_vod_thread_pool_init（线程数，0=在 poller 上同步执行）。
 */
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;

typedef void (*zms_vod_thread_pool_work_fn)(void *user);
typedef void (*zms_vod_thread_pool_on_io_fn)(void *user);

/**
 * 初始化 VOD 磁盘任务线程池（会先销毁旧池）。
 * @param thread_count 线程数；<=0 表示不建池，VOD 文件操作在 poller 上同步执行
 */
ZMS_API void zms_vod_thread_pool_init(int thread_count);
/** 销毁线程池；未初始化时无操作。 */
ZMS_API void zms_vod_thread_pool_fini(void);
/** @return 非零表示线程池已启用（文件操作异步到工作线程）。 */
ZMS_API int zms_vod_thread_pool_enabled(void);

/**
 * @param io 目标 poller（通常为 zn_tcp_session_poller）
 * @note user 须在 on_io 返回前有效；on_io 内应检查连接是否仍有效
 */
ZMS_API zn_err_t zms_vod_thread_pool_run(struct zn_poller *io, zms_vod_thread_pool_work_fn work,
                                          zms_vod_thread_pool_on_io_fn on_io, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_UTIL_THREAD_POOL_H */
