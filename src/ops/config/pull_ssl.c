/**
 * @file pull_ssl.c
 * @brief 拉流 TLS 配置辅助。
 */
#include "zcs/ops/config/pull_ssl.h"
#include "zcs/ops/config/ssl_config.h"
#include "zn/net/ssl.h"
#include <stdlib.h>

static struct zn_ssl_ctx *s_pull_ssl;
static int s_pull_ssl_tried;

struct zn_ssl_ctx *zcs_pull_ssl_ctx(const zcs_config *cfg)
{
#if !defined(ZN_HAVE_OPENSSL) || !ZN_HAVE_OPENSSL
    (void)cfg;
    return NULL;
#else
    if (s_pull_ssl || s_pull_ssl_tried) {
        return s_pull_ssl;
    }
    s_pull_ssl_tried = 1;
    zn_ssl_config_t sc;
    zcs_ssl_config_from_zcs(&sc, cfg);
    s_pull_ssl = zn_ssl_ctx_create(&sc);
    return s_pull_ssl;
#endif
}
