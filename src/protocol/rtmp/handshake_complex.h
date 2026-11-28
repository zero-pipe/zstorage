/**
 * @file handshake_complex.h
 * @brief Adobe complex RTMP 握手数学。
 */
#ifndef ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H
#define ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H

#include "zcs/zcs_export.h"

#include <stdint.h>

#define ZCS_RTMP_HS_SIZE 1536

ZCS_LOCAL int zcs_rtmp_handshake_detect_complex(const uint8_t c1[ZCS_RTMP_HS_SIZE]);
ZCS_LOCAL int zcs_rtmp_handshake_build_complex(const uint8_t c1[ZCS_RTMP_HS_SIZE],
                                               uint8_t s1[ZCS_RTMP_HS_SIZE],
                                               uint8_t s2[ZCS_RTMP_HS_SIZE]);
ZCS_LOCAL int zcs_rtmp_handshake_validate_c2(const uint8_t s1[ZCS_RTMP_HS_SIZE],
                                             const uint8_t c2[ZCS_RTMP_HS_SIZE]);

#endif /* ZCS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H */
