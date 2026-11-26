/**
 * @file handshake_crypto.h
 * @brief RTMP 握手 HMAC-SHA256。
 */
#ifndef ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H
#define ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H

#include "zms/zms_export.h"

#include <stddef.h>
#include <stdint.h>

ZMS_LOCAL void zms_rtmp_handshake_hmac_sha256(const uint8_t *key, size_t key_len,
                                              const uint8_t *data, size_t data_len, uint8_t out[32]);

#endif /* ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_CRYPTO_H */
