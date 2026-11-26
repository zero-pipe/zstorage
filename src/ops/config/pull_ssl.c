/**
 * @file pull_ssl.c
 * @brief 拉流 TLS 配置辅助。
 */
#include "zms/ops/config/pull_ssl.h"
#include "zms/ops/config/ssl_config.h"
#include "zn/net/ssl.h"
#include <stdlib.h>

static struct zn_ssl_ctx *s_pull_ssl;
static int s_pull_ssl_tried;

struct zn_ssl_ctx *zms_pull_ssl_ctx(const zms_config *cfg)
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
    zms_ssl_config_from_zms(&sc, cfg);
    s_pull_ssl = zn_ssl_ctx_create(&sc);
    return s_pull_ssl;
#endif
}
