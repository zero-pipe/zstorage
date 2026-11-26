/**
 * @file ssl_config.h
 * @brief SSL 配置。
 */
#ifndef ZMS_OPS_SERVICE_SSL_CONFIG_H
#define ZMS_OPS_SERVICE_SSL_CONFIG_H

#include "zms/ops/config/config.h"
#include "zn/net/ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 将 ZMS [ssl] 配置填入 zn_ssl_config（仅当 ZN_HAVE_OPENSSL 时有效） */
static inline void zms_ssl_config_from_zms(zn_ssl_config_t *out, const zms_config *cfg)
{
    if (!out) {
        return;
    }
    out->ca_file = NULL;
    out->verify_peer = 0;
    out->client_cert_file = NULL;
    out->client_key_file = NULL;
    /* 服务端字段必须显式清零：zn_ssl_ctx_create 会按 server_mode 分支读取，
     * 未初始化的栈内容会被当成有效指针解引用（启动即崩溃）。 */
    out->server_mode = 0;
    out->cert_file = NULL;
    out->key_file = NULL;
    if (!cfg) {
        return;
    }
    if (cfg->ssl.ca_file[0]) {
        out->ca_file = cfg->ssl.ca_file;
    }
    out->verify_peer = cfg->ssl.verify_peer;
    if (cfg->ssl.client_cert[0]) {
        out->client_cert_file = cfg->ssl.client_cert;
    }
    if (cfg->ssl.client_key[0]) {
        out->client_key_file = cfg->ssl.client_key;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ZMS_OPS_SERVICE_SSL_CONFIG_H */
