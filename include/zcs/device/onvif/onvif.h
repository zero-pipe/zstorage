/**
 * @file onvif.h
 * @brief ONVIF 客户端（M1）：WS-Discovery、设备信息、Media1 profiles/取流地址、PTZ。
 *
 * 设计约束：
 *   - 进程内、无第三方依赖（自写 SOAP/XML 层）；
 *   - 同步阻塞接口，调用方放在独立线程/任务里使用；
 *   - 只负责"找到并管理 IPC"，媒体统一走 ZCS 现有拉流链路。
 */
#ifndef ZCS_DEVICE_ONVIF_ONVIF_H
#define ZCS_DEVICE_ONVIF_ONVIF_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_ONVIF_XADDR_MAX 256
#define ZCS_ONVIF_URI_MAX 512
#define ZCS_ONVIF_NAME_MAX 128

typedef struct zcs_onvif_device {
    /** device_service 地址（来自发现或手动配置） */
    char xaddr[ZCS_ONVIF_XADDR_MAX];
    char uuid[64];
    char name[ZCS_ONVIF_NAME_MAX];
    char hardware[64];
    char manufacturer[ZCS_ONVIF_NAME_MAX];
    char model[ZCS_ONVIF_NAME_MAX];
    char firmware[64];
    char serial[64];
    /** 能力（GetCapabilities/GetServices） */
    char media_xaddr[ZCS_ONVIF_XADDR_MAX];
    char ptz_xaddr[ZCS_ONVIF_XADDR_MAX];
    /** 发现返回的 scopes 原文 */
    char scopes[512];
} zcs_onvif_device;

typedef struct zcs_onvif_profile {
    char token[64];
    char name[ZCS_ONVIF_NAME_MAX];
    char encoding[16];
    int width;
    int height;
    int fps;
    /** GetStreamUri 结果（RTSP URL） */
    char uri[ZCS_ONVIF_URI_MAX];
} zcs_onvif_profile;

typedef struct zcs_onvif_client zcs_onvif_client;

/** WS-Discovery 探测（组播 239.255.255.250:3702），结果按 xaddr 去重。 */
ZCS_API int zcs_onvif_discover(int timeout_ms, zcs_onvif_device *out, size_t cap, size_t *count);

/** 连接设备：创建设备句柄并刷新设备信息/能力。user/pass 可为 NULL（匿名）。 */
ZCS_API zcs_onvif_client *zcs_onvif_connect(const char *device_xaddr, const char *user,
                                            const char *pass, int timeout_ms);

/** 重新读取设备信息与能力。 */
ZCS_API int zcs_onvif_refresh(zcs_onvif_client *c, zcs_onvif_device *out);

/** 读取 profiles，并逐个调用 GetStreamUri 填充 uri。 */
ZCS_API int zcs_onvif_get_profiles(zcs_onvif_client *c, zcs_onvif_profile *out, size_t cap,
                                   size_t *count);

/** 取客户端缓存的设备信息（借用指针，随 close 失效）；c 为 NULL 时返回 NULL。 */
ZCS_API const zcs_onvif_device *zcs_onvif_device_info(const zcs_onvif_client *c);

/** PTZ 连续移动：pan/tilt/zoom 取 -1.0..1.0（0 表示该轴不动）。 */
ZCS_API int zcs_onvif_ptz_move(zcs_onvif_client *c, const char *profile_token, float pan,
                               float tilt, float zoom);
/** 发送 PTZ 停止命令；无 PTZ 能力、参数非法或网络失败返回 -1。 */
ZCS_API int zcs_onvif_ptz_stop(zcs_onvif_client *c, const char *profile_token);

/** 关闭并释放客户端句柄；对 NULL 安全。 */
ZCS_API void zcs_onvif_close(zcs_onvif_client *c);

/**
 * 若 RTSP URL 未带 userinfo 且提供了凭据，则插入 user:pass@（URL 编码）。
 * 无凭据或已带 userinfo 时原样输出。
 */
ZCS_API int zcs_onvif_uri_with_auth(const char *uri, const char *user, const char *pass, char *out,
                                    size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_ONVIF_ONVIF_H */
