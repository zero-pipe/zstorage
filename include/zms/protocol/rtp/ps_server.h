#ifndef ZMS_PROTOCOL_RTP_PS_SERVER_H
#define ZMS_PROTOCOL_RTP_PS_SERVER_H

/**
 * @file ps_server.h
 * @brief GB28181 媒体面：UDP/TCP RTP/PS 接入槽（信令在 Go 侧）。
 */
#include "zms/core/stream/hub.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_poller_pool;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_pool zn_poller_pool;

#define ZMS_RTP_PS_DEFAULT_PT 96

/** 对齐 ZLM openRtpServer tcp_mode：0=UDP，1=TCP被动(平台listen)，2=TCP主动(平台connect) */
#define ZMS_RTP_PS_TCP_UDP 0
#define ZMS_RTP_PS_TCP_PASSIVE 1
#define ZMS_RTP_PS_TCP_ACTIVE 2

typedef struct zms_rtp_ps_server_slot zms_rtp_ps_server_slot;

typedef struct zms_rtp_ps_server_open_opts {
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const char *vhost;
    const char *app;
    const char *stream;
    /** 0 = kernel picks ephemeral port */
    uint16_t port;
    int payload_type;
    /** 0/1/2 ZMS_RTP_PS_TCP_* */
    int tcp_mode;
/** 非零时仅接受此 SSRC 的 RTP */
    uint32_t ssrc;
    int enable_ssrc_filter;
    /**
     * 非零表示该槽位由信令层（GB28181 INVITE 等）协商授权，开槽时跳过 token 验签。
     * 外部可直接访问的入口（webapi openRtpServer）必须保持 0。
     */
    int signaling_authorized;
    /** 调用方 URL 的查询串（鉴权 token 等）；external 开槽时用于验签。 */
    const char *params;
} zms_rtp_ps_server_open_opts;

/**
 * 生成槽位注册键 "vhost/app/stream"。vhost 为空用 "__defaultVhost__"，app 为空用 "live"。
 * key_cap 过小或 key 为 NULL 时直接返回，不做截断保证。
 */
ZMS_API void zms_rtp_ps_server_make_key(const char *vhost, const char *app, const char *stream,
                                        char *key, size_t key_cap);

/**
 * 开一个 RTP/PS 接入槽并起监听。
 *
 * 先做发布鉴权（signaling_authorized 非 0 时跳过 token 验签，仅供信令层/GB28181 INVITE 使用），
 * 按 vhost/app/stream 复用已存在且配置相同的槽位，避免重复 open 的 close+重建竞态。
 *
 * @param opts     开槽参数；poller 与 stream 必填，port=0 由内核选端口，tcp_mode<0 按 UDP。
 * @param out_slot 输出槽位（借用，归注册表所有，经 close 释放）。
 * @param out_port 输出实际监听端口；可为 NULL（仍会校验 out_slot）。
 * @return ZN_OK；鉴权失败 ZN_ERR_DENIED；参数/状态/IO 错误返回对应错误码。
 */
ZMS_API zn_err_t zms_rtp_ps_server_open(const zms_rtp_ps_server_open_opts *opts,
                                         zms_rtp_ps_server_slot **out_slot, uint16_t *out_port);

/** TCP-ACTIVE：open OK 后平台主动连接摄像头 media 地址（对齐 ZLM connectRtpServer） */
ZMS_API zn_err_t zms_rtp_ps_server_connect(zn_poller *poller, const char *vhost, const char *app,
                                            const char *stream, const char *host, uint16_t port);

/** 关闭槽位：停监听、销毁媒体管线并从注册表摘除；对 NULL 安全。 */
ZMS_API void zms_rtp_ps_server_close(zms_rtp_ps_server_slot *slot);

/** 按 make_key 生成的键查找槽位（借用指针，随槽位关闭失效）；未找到返回 NULL。 */
ZMS_API zms_rtp_ps_server_slot *zms_rtp_ps_server_find_by_key(const char *key);

/** 取槽位关联的媒体源（借用，生命周期随槽位）；slot 为 NULL 时返回 NULL。 */
ZMS_API zms_media_source *zms_rtp_ps_server_source(const zms_rtp_ps_server_slot *slot);

/** 取槽位实际监听端口；slot 为 NULL 时返回 0。 */
ZMS_API uint16_t zms_rtp_ps_server_port(const zms_rtp_ps_server_slot *slot);

/** 取槽位 app（借用字符串，勿释放）；slot 为 NULL 时返回空串。 */
ZMS_API const char *zms_rtp_ps_server_app(const zms_rtp_ps_server_slot *slot);
/** 取槽位 stream（借用字符串，勿释放）；slot 为 NULL 时返回空串。 */
ZMS_API const char *zms_rtp_ps_server_stream(const zms_rtp_ps_server_slot *slot);

/** 取槽位协商的 RTP payload type；slot 为 NULL 时返回默认值 96。 */
ZMS_API int zms_rtp_ps_server_payload_type(const zms_rtp_ps_server_slot *slot);

typedef int (*zms_rtp_ps_server_visit_fn)(const char *key, zms_rtp_ps_server_slot *slot,
                                          void *user);
/**
 * 遍历所有已注册的槽位。
 * @param fn 回调；返回非 0 时提前终止。
 * @return 已访问的槽位数。
 * 注意：注册表未内置加锁，调用方需自行串行化（通常在信令/管理线程）。
 */
ZMS_API int zms_rtp_ps_server_foreach(zms_rtp_ps_server_visit_fn fn, void *user);

void zms_rtp_ps_register(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_RTP_PS_SERVER_H */
