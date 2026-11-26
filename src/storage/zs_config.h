/**
 * @file zs_config.h
 * @brief 配置桥：把 [storage] 段的 INI 配置翻译成存储层参数。
 *
 * 放在 storage 层而不是 ops 层，是为了让 config.h 不反向依赖存储头文件；
 * 这里只做单位换算与默认值兜底，不做 IO。
 */
#ifndef ZMS_SRC_STORAGE_ZS_CONFIG_H
#define ZMS_SRC_STORAGE_ZS_CONFIG_H

#include "storage/zs_pool.h"
#include "storage/zs_writer.h"
#include "zms/ops/config/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZS_KB (1024ULL)
#define ZS_MB (1024ULL * ZS_KB)
#define ZS_GB (1024ULL * ZS_MB)

/** [storage] -> zs_pool_opts（0/负值按默认值处理）。 */
void zs_pool_opts_from_config(zs_pool_opts *out, const zms_storage_config *cfg);

/** [storage] -> zs_writer_opts，绑定到一个通道。 */
void zs_writer_opts_from_config(zs_writer_opts *out, const zms_storage_config *cfg, zs_pool *pool,
                                uint32_t channel_id, const char *stream);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_STORAGE_ZS_CONFIG_H */
