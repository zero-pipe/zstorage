/**
 * @file handshake_crypto.h
 * @brief RTMP 握手 HMAC-SHA256。
 */
#ifndef ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H
#define ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H

#include "zcs/zcs_export.h"

#include <stddef.h>
#include <stdint.h>

ZCS_LOCAL void zcs_rtmp_handshake_hmac_sha256(const uint8_t *key, size_t key_len,
                                              const uint8_t *data, size_t data_len, uint8_t out[32]);

#endif /* ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H */
