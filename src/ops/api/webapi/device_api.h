/**
 * @file device_api.h
 * @brief /index/api/device/*：协议无关的设备能力视图。
 */
#ifndef ZMS_SRC_OPS_API_WEBAPI_DEVICE_API_H
#define ZMS_SRC_OPS_API_WEBAPI_DEVICE_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @return >0 已写 body。 */
int zms_webapi_device_capabilities(char *body, size_t body_cap);
int zms_webapi_device_events(char *body, size_t body_cap);
int zms_webapi_device_ptz(const char *channel, float pan, float tilt, float zoom, int stop,
                          char *body, size_t body_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_OPS_API_WEBAPI_DEVICE_API_H */
