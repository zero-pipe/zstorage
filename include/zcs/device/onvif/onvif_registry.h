/**
 * @file onvif_registry.h
 * @brief ONVIF 设备接入登记：设备 → 拉流源（pull proxy）→ 媒体注册表。
 *
 * 只负责控制面映射；媒体仍由 zcs_live_pull_proxy + RTSP 客户端统一处理。
 */
#ifndef ZCS_DEVICE_ONVIF_ONVIF_REGISTRY_H
#define ZCS_DEVICE_ONVIF_ONVIF_REGISTRY_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller_pool;
typedef struct zn_poller_pool zn_poller_pool;

typedef struct zcs_onvif_entry_info {
    char key[256]; /**< vhost/app/stream */
    char addr[256];
    char profile[64];
    char uri[512];
    char vhost[64];
    char app[64];
    char stream[64];
    int online; /**< 源当前是否在线 */
} zcs_onvif_entry_info;

typedef int (*zcs_onvif_registry_visit_fn)(const zcs_onvif_entry_info *info, void *user);

/** 进程生命周期内调用一次（幂等）。 */
ZCS_API void zcs_onvif_registry_init(void);
/** 释放所有登记项（停止拉流代理）并销毁互斥量；未初始化或重复调用无操作。 */
ZCS_API void zcs_onvif_registry_fini(void);

/**
 * 用已解析的 RTSP 地址创建拉流代理并登记。
 * 同 key 已存在时先拆除旧的。成功返回 0，key_out 为本地 key。
 */
ZCS_API int zcs_onvif_registry_add(const char *addr, const char *user, const char *pass,
                                   const char *profile, const char *uri, const char *vhost,
                                   const char *app, const char *stream, zn_poller_pool *pool,
                                   char *key_out, size_t key_cap);

/** 按 key 删除（停止拉流代理）。返回 1=已删除，0=不存在。 */
ZCS_API int zcs_onvif_registry_del_key(const char *key);
/** 按设备地址删除所有 profile。*removed 为删除数量。 */
ZCS_API int zcs_onvif_registry_del_addr(const char *addr, int *removed);
/** 遍历（回调中不得再次调用 registry 修改接口）。 */
ZCS_API int zcs_onvif_registry_foreach(zcs_onvif_registry_visit_fn fn, void *user);
/** 当前登记条目数；未初始化时返回 0。 */
ZCS_API int zcs_onvif_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_ONVIF_ONVIF_REGISTRY_H */
