/**
 * @file pull_ssl.h
 * @brief 拉流 TLS 配置。
 */
#ifndef ZCS_OPS_SERVICE_PULL_SSL_H
#define ZCS_OPS_SERVICE_PULL_SSL_H

#include "zcs/ops/config/config.h"
#include "zcs/zcs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

struct zn_ssl_ctx;

/** 按 [ssl] 配置返回进程内共享 TLS 上下文（懒创建，勿 destroy） */
ZCS_API struct zn_ssl_ctx *zcs_pull_ssl_ctx(const zcs_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_OPS_SERVICE_PULL_SSL_H */
