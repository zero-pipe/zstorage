/**
 * @file rtsp.h
 * @brief RTSP 方法/消息定义。
 */
#ifndef ZCS_PROTOCOL_RTSP_RTSP_H
#define ZCS_PROTOCOL_RTSP_RTSP_H

#include "zcs/zcs_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_RTSP_DEFAULT_PORT 554u

typedef enum zcs_rtsp_transport {
    ZCS_RTSP_TRANSPORT_TCP = 0,
    ZCS_RTSP_TRANSPORT_UDP = 1,
    ZCS_RTSP_TRANSPORT_MULTICAST = 2,
} zcs_rtsp_transport;

typedef enum zcs_rtsp_method {
    ZCS_RTSP_OPTIONS = 0,
    ZCS_RTSP_DESCRIBE,
    ZCS_RTSP_SETUP,
    ZCS_RTSP_PLAY,
    ZCS_RTSP_PAUSE,
    ZCS_RTSP_TEARDOWN,
    ZCS_RTSP_GET_PARAMETER,
    ZCS_RTSP_SET_PARAMETER,
    ZCS_RTSP_ANNOUNCE,
    ZCS_RTSP_RECORD,
    ZCS_RTSP_UNKNOWN,
} zcs_rtsp_method;

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_RTSP_H */
