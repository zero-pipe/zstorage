/**
 * @file gb28181.h
 * @brief GB/T 28181 设备端（IPC 侧）：注册到上级平台并推送 PS/RTP 流。
 *
 * 能力范围（首期）：
 *   - REGISTER / 注销 / Keepalive；
 *   - Catalog / DeviceInfo / DeviceStatus / ConfigDownload 查询应答；
 *   - DeviceControl（PTZ 透传/录像控制/关键帧请求）与 RecordInfo 录像查询；
 *   - INVITE 实时点播（UDP / TCP 主被动）与 Playback 回放；
 *   - Alarm 报警主动上报。
 *
 * 媒体面完全复用 ZMS 现有链路：gop_queue → PS 打包 → RTP 发送；
 * 不引入 Go/Python sidecar，信令与媒体都在本进程内。
 */
#ifndef ZMS_DEVICE_GB28181_GB28181_H
#define ZMS_DEVICE_GB28181_GB28181_H

#include "zms/zms_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_GB28181_ID_MAX 32
#define ZMS_GB28181_NAME_MAX 128
#define ZMS_GB28181_CHANNEL_MAX 64

struct zn_poller_pool;
typedef struct zn_poller_pool zn_poller_pool;

/** 启动参数；字符串在 start 内部拷贝。 */
typedef struct zms_gb28181_opts {
    /** 上级平台 20 位 ID 与 SIP 域；domain 一般为 platform_id 前 10 位 */
    const char *platform_id;
    const char *platform_domain;
    const char *platform_ip;
    unsigned platform_port;

    /** 本机设备 20 位 ID 与描述 */
    const char *device_id;
    const char *device_name;
    const char *manufacturer;
    const char *model;
    const char *firmware;

    /** Digest 用户名（空则用 device_id）/ 密码 */
    const char *username;
    const char *password;

    /** 本机 SIP 监听地址；local_ip 为空时按平台地址自动探测 */
    const char *local_ip;
    unsigned local_port;
    /** udp（默认）/ tcp / tls（设备作为 SIP 客户端连接平台） */
    const char *sip_transport;
    /** client（默认）/ server（tcp/tls 时平台连入） */
    const char *sip_mode;
    const char *tls_ca;
    const char *tls_cert;
    const char *tls_key;
    int tls_verify_peer;

    int register_expires;   /**< 默认 3600 */
    int keepalive_interval; /**< 秒，默认 60 */

    /**
     * 通道映射：分号分隔的 `<20位通道ID>:<vhost>/<app>/<stream>`。
     * vhost 可省略（视为默认 vhost）。通道 ID 也允许 32 位以内。
     */
    const char *channels;

    /** 录像根目录（RecordInfo/回放）；空则使用 [record] root */
    const char *record_root;

    /** 服务器 poller pool（可为空：模块内部自建单线程 poller） */
    zn_poller_pool *poller_pool;
} zms_gb28181_opts;

/** 启动设备端（幂等）。成功返回 0。 */
ZMS_API int zms_gb28181_start(const zms_gb28181_opts *opts);
/** 停止：注销、停止全部会话与线程。 */
ZMS_API void zms_gb28181_stop(void);
/** @return 非零表示设备端已启动（start 之后、stop 之前）。 */
ZMS_API int zms_gb28181_running(void);
/** @return 非零表示已向 SIP 平台注册成功。 */
ZMS_API int zms_gb28181_registered(void);

typedef struct zms_gb28181_channel_info {
    char id[ZMS_GB28181_ID_MAX];
    char name[ZMS_GB28181_NAME_MAX];
    char vhost[128];
    char app[64];
    char stream[128];
    int online;
} zms_gb28181_channel_info;

/** 遍历通道映射（返回写入数量）。 */
ZMS_API size_t zms_gb28181_channels(zms_gb28181_channel_info *out, size_t cap);

/** 当前点播会话数。 */
ZMS_API int zms_gb28181_session_count(void);

/**
 * 主动上报报警（MANSCDP Notify Alarm）。
 * @param channel_id 空则用设备 ID
 * @param priority 1-4（1 最高）
 * @param method 报警方式（如 5=视频报警，6=设备报警）
 * @return 0 成功（已发出）
 */
ZMS_API int zms_gb28181_alarm(const char *channel_id, int priority, int method,
                              const char *description);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_DEVICE_GB28181_GB28181_H */
