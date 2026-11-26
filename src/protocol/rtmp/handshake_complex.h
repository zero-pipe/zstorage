/**
 * @file handshake_complex.h
 * @brief Adobe complex RTMP 握手数学。
 */
#ifndef ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H
#define ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H

#include "zms/zms_export.h"

#include <stdint.h>

#define ZMS_RTMP_HS_SIZE 1536

ZMS_LOCAL int zms_rtmp_handshake_detect_complex(const uint8_t c1[ZMS_RTMP_HS_SIZE]);
ZMS_LOCAL int zms_rtmp_handshake_build_complex(const uint8_t c1[ZMS_RTMP_HS_SIZE],
                                               uint8_t s1[ZMS_RTMP_HS_SIZE],
                                               uint8_t s2[ZMS_RTMP_HS_SIZE]);
ZMS_LOCAL int zms_rtmp_handshake_validate_c2(const uint8_t s1[ZMS_RTMP_HS_SIZE],
                                             const uint8_t c2[ZMS_RTMP_HS_SIZE]);

#endif /* ZMS_SRC_PROTOCOL_RTMP_HANDSHAKE_COMPLEX_H */
