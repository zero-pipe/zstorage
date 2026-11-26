/**
 * @file device_event.h
 * @brief 设备事件总线：报警/在线/离线/流状态/PTZ，供 ONVIF/GB28181/API 共用。
 */
#ifndef ZMS_DEVICE_CORE_DEVICE_EVENT_H
#define ZMS_DEVICE_CORE_DEVICE_EVENT_H

#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_DEVICE_EVENT_RING 64
#define ZMS_DEVICE_EVENT_SUBSCRIBERS 8

typedef enum zms_device_event_type {
    ZMS_DEV_EVENT_ALARM = 0,
    ZMS_DEV_EVENT_MOTION,
    ZMS_DEV_EVENT_ONLINE,
    ZMS_DEV_EVENT_OFFLINE,
    ZMS_DEV_EVENT_STREAM_START,
    ZMS_DEV_EVENT_STREAM_STOP,
    ZMS_DEV_EVENT_PTZ,
} zms_device_event_type;

typedef struct zms_device_event {
    int type;
    char device_id[64];
    char channel_id[64];
    int priority;
    uint64_t time_ms;
    char message[128];
} zms_device_event;

typedef void (*zms_device_event_cb)(const zms_device_event *ev, void *user);

/** 初始化设备事件总线（懒建互斥量）；重复调用无副作用。 */
void zms_device_event_init(void);
/** 清空环形缓冲与订阅者并销毁互斥量；进程退出时调用。 */
void zms_device_event_fini(void);

/** 发布事件（线程安全；回调在锁外执行）。 */
ZMS_API void zms_device_event_publish(const zms_device_event *ev);

/** 最近事件（新→旧）。返回写入数量。 */
ZMS_API size_t zms_device_event_list(zms_device_event *out, size_t cap);

/** 订阅/取消；最多 8 个订阅者。 */
ZMS_API int zms_device_event_subscribe(zms_device_event_cb cb, void *user);
/** 取消订阅（按 cb+user 精确匹配）；返回 0 成功、-1 未找到或参数非法。 */
ZMS_API int zms_device_event_unsubscribe(zms_device_event_cb cb, void *user);

/** 事件类型的稳定小写名称（如 "alarm"/"ptz"）；未知返回 "unknown"。 */
ZMS_API const char *zms_device_event_type_name(int type);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_DEVICE_CORE_DEVICE_EVENT_H */
