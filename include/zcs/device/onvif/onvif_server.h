/**
 * @file onvif_server.h
 * @brief ONVIF 设备端（M4）：让 ZCS 作为 IPC/NVR 被第三方平台发现与取流。
 *
 * 能力范围（首期）：
 *   - WS-Discovery 应答（Hello/ProbeMatch/Bye）
 *   - Device 服务：GetSystemDateAndTime / GetDeviceInformation / GetCapabilities /
 *     GetServices / GetNetworkInterfaces
 *   - Media1 服务：GetProfiles / GetVideoSources / GetStreamUri
 * 不带 PTZ / 快照 / Events；不改变媒体路径（GetStreamUri 返回本机 RTSP 地址）。
 */
#ifndef ZCS_DEVICE_ONVIF_ONVIF_SERVER_H
#define ZCS_DEVICE_ONVIF_ONVIF_SERVER_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
typedef struct zn_poller zn_poller;

typedef struct zcs_onvif_server_opts {
    /** WS-Discovery 组播 socket 注册到的 poller（必填，discovery=1 时使用） */
    zn_poller *poller;
    /** 对外公布的主机名/IP（一般取 [general] externIP） */
    const char *advertise_host;
    unsigned http_port;
    unsigned rtsp_port;
    /** 设备名（WS-Discovery Scopes / DeviceInformation） */
    const char *name;
    /** 设备 UUID（urn:uuid:...）；空则自动生成 */
    const char *uuid;
    /** 是否启用 WS-Discovery 应答（0=仅 HTTP 服务） */
    int discovery;
} zcs_onvif_server_opts;

/** 注册 /onvif/* HTTP 路由（幂等；始终注册，未启用时返回 404）。 */
ZCS_API void zcs_onvif_server_routes_register(void);

/** 启动设备端（保存配置；discovery=1 时启动组播应答线程）。 */
ZCS_API int zcs_onvif_server_start(const zcs_onvif_server_opts *opts);
/** 停止设备端：拆除 WS-Discovery 组播并关闭服务；幂等。 */
ZCS_API void zcs_onvif_server_stop(void);
/** 查询设备端是否处于启用状态（非 0 表示已 start 且未 stop）。 */
ZCS_API int zcs_onvif_server_running(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_ONVIF_ONVIF_SERVER_H */
