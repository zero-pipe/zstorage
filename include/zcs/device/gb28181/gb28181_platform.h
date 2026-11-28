/**
 * @file gb28181_platform.h
 * @brief GB28181 平台端（最小集）：接受设备 REGISTER/心跳/查询应答，维护设备目录。
 *
 * 与设备端（gb28181.h）可在同一进程内共存，使用不同端口与配置节。
 * 媒体面复用 ZCS 现有 rtp-ps 接入链路（后续 INVITE 阶段接入）。
 */
#ifndef ZCS_DEVICE_GB28181_GB28181_PLATFORM_H
#define ZCS_DEVICE_GB28181_GB28181_PLATFORM_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_GB_PLATFORM_ID_MAX 32
#define ZCS_GB_PLATFORM_CHANNEL_MAX 64

typedef struct zcs_gb28181_platform_device {
    char id[ZCS_GB_PLATFORM_ID_MAX];
    char ip[64];
    unsigned port;
    int online;
    uint64_t last_seen_ms;
    char name[128];
    char manufacturer[64];
    char model[64];
    char firmware[32];
    int channel_count;
} zcs_gb28181_platform_device;

typedef struct zcs_gb28181_platform_channel {
    char device_id[ZCS_GB_PLATFORM_ID_MAX];
    char id[ZCS_GB_PLATFORM_ID_MAX];
    char name[128];
    int online;
} zcs_gb28181_platform_channel;

struct zn_poller_pool;
typedef struct zn_poller_pool zn_poller_pool;

typedef struct zcs_gb28181_platform_opts {
    const char *id;     /**< 平台 20 位 ID */
    const char *domain; /**< SIP 域（空=id 前 10 位） */
    const char *password;
    /** 对外公布 IP；空=按设备地址自动探测 */
    const char *advertise_ip;
    unsigned listen_port;
    /** udp / tcp / tls（tcp/tls 为服务端，设备连入） */
    const char *transport;
    const char *tls_cert;
    const char *tls_key;
    const char *tls_ca;
    int tls_verify_peer;
    /** 心跳超时（秒），0=默认 180 */
    int keepalive_timeout_sec;
    /** INVITE 取流写入的 vhost/app（stream=通道 ID） */
    const char *media_vhost;
    const char *media_app;
    zn_poller_pool *poller_pool;
} zcs_gb28181_platform_opts;

/**
 * 启动平台端 SIP 服务，接受设备 REGISTER/心跳/查询应答并维护设备目录。
 * @param opts 平台参数；id 必填，其余留空取默认（transport=udp、listen_port=5060、
 *             media_app=gb28181、keepalive=180s）。若已在运行会先 stop 再重启。
 * @return 0 启动成功；-1 参数非法或资源创建失败。
 */
ZCS_API int zcs_gb28181_platform_start(const zcs_gb28181_platform_opts *opts);
/** 停止平台端服务并释放资源；未运行时无操作。 */
ZCS_API void zcs_gb28181_platform_stop(void);
/** 查询平台端是否正在运行。 */
ZCS_API int zcs_gb28181_platform_running(void);

/** 设备列表（拷贝）。返回写入数量。 */
ZCS_API size_t zcs_gb28181_platform_devices(zcs_gb28181_platform_device *out, size_t cap);
/** 指定设备的通道目录（来自 Catalog 应答）。 */
ZCS_API size_t zcs_gb28181_platform_channels(const char *device_id,
                                             zcs_gb28181_platform_channel *out, size_t cap);
/** 主动查询：cmd = Catalog / DeviceInfo / DeviceStatus。返回 0 已发送。 */
ZCS_API int zcs_gb28181_platform_query(const char *device_id, const char *cmd);

/** 取流会话（INVITE） */
typedef struct zcs_gb28181_platform_session {
    char call_id[96];
    char device_id[ZCS_GB_PLATFORM_ID_MAX];
    char channel_id[ZCS_GB_PLATFORM_ID_MAX];
    char vhost[64];
    char app[64];
    char stream[128];
    unsigned ssrc;
    unsigned local_port;
    int active;
} zcs_gb28181_platform_session;

/**
 * 向设备通道发起 INVITE（UDP/PS，写入 media_vhost/app，stream=channel_id）。
 * @param tcp 0=UDP；1=TCP 被动（平台 listen）；2=TCP 主动（平台 connect）
 * @return 0 已发送，-1 失败
 */
ZCS_API int zcs_gb28181_platform_invite(const char *device_id, const char *channel_id,
                                        const char *app, const char *stream, int tcp);
/** 按 call_id 结束会话（发送 BYE）。 */
ZCS_API int zcs_gb28181_platform_bye(const char *call_id);
/** 当前取流会话。 */
ZCS_API size_t zcs_gb28181_platform_sessions(zcs_gb28181_platform_session *out, size_t cap);

/** 云台控制（归一化 -1..1；stop=1 停止）。 */
ZCS_API int zcs_gb28181_platform_ptz(const char *device_id, const char *channel_id, float pan,
                                     float tilt, float zoom, int stop);

typedef struct zcs_gb28181_platform_record {
    char channel_id[ZCS_GB_PLATFORM_ID_MAX];
    char name[128];
    char file_path[512];
    char start_time[32];
    char end_time[32];
    char type[16];
} zcs_gb28181_platform_record;

/** 向设备查询录像（异步；结果经 platform/records 读取）。 */
ZCS_API int zcs_gb28181_platform_record_query(const char *device_id, const char *channel_id,
                                              const char *start_time, const char *end_time);
/**
 * 拷贝指定设备（可按 channel_id 过滤）的录像列表到 out。
 * @param channel_id 为 NULL/空串时返回该设备全部通道的录像
 * @return 写入 out 的条目数（受 cap 限制）；device_id/out 为空或 cap=0 返回 0。
 */
ZCS_API size_t zcs_gb28181_platform_records(const char *device_id, const char *channel_id,
                                            zcs_gb28181_platform_record *out, size_t cap);

typedef struct zcs_gb28181_platform_alarm {
    char device_id[ZCS_GB_PLATFORM_ID_MAX];
    char channel_id[ZCS_GB_PLATFORM_ID_MAX];
    int priority;
    int method;
    uint64_t time_ms;
    char description[128];
} zcs_gb28181_platform_alarm;

/** 拷贝当前告警列表到 out，返回写入数量（受 cap 限制）；out 为空或 cap=0 返回 0。 */
ZCS_API size_t zcs_gb28181_platform_alarms(zcs_gb28181_platform_alarm *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_GB28181_GB28181_PLATFORM_H */
