/**
 * @file device_ptz.h
 * @brief 归一化 PTZ 命令：ONVIF ContinuousMove / GB28181 PTZCmd 共用。
 */
#ifndef ZCS_DEVICE_CORE_DEVICE_PTZ_H
#define ZCS_DEVICE_CORE_DEVICE_PTZ_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_device_ptz_cmd {
    char channel_id[64];
    /** -1.0 .. 1.0；0 表示该轴不动 */
    float pan;
    float tilt;
    float zoom;
    int stop;
} zcs_device_ptz_cmd;

/** 返回 0=已执行；其他=失败。 */
typedef int (*zcs_device_ptz_handler)(const zcs_device_ptz_cmd *cmd, void *user);

void zcs_device_ptz_init(void);
void zcs_device_ptz_fini(void);

/** 注册/清除后端（如 ONVIF 客户端、硬件云台）。 */
ZCS_API int zcs_device_ptz_set_handler(zcs_device_ptz_handler fn, void *user);

/** 是否已注册后端（能力宣告用）。 */
ZCS_API int zcs_device_ptz_has_backend(void);

/** 分发命令；无后端时记录事件并视为已接受。 */
ZCS_API int zcs_device_ptz_dispatch(const zcs_device_ptz_cmd *cmd);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_CORE_DEVICE_PTZ_H */
