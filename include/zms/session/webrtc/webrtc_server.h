/**
 * @file webrtc_server.h
 * @brief WebRTC 服务：ICE/DTLS 配置、会话统计与主动客户端。
 */
#ifndef ZMS_SESSION_WEBRTC_SERVER_H
#define ZMS_SESSION_WEBRTC_SERVER_H

#include "zms/zms_export.h"
#include "zn/poller/poller.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_webrtc_server zms_webrtc_server;

/** 仅前向声明；调用方负责包含 HTTP 会话内部头。 */
struct zms_http_session;

typedef struct zms_webrtc_server_opts {
    zn_poller *poller;
    const char *host;
    /** ICE candidate / SDP c= 地址（默认 127.0.0.1）。 */
    const char *advertise_host;
    uint16_t port_min;
    uint16_t port_max;
    const char *stun_server;
    const char *turn_server;
    const char *turn_user;
    const char *turn_pass;
    int signaling_enable;
    const char *signaling_url;
    const char *signaling_room;
    const char *signaling_token;
    const char *signaling_app;
    const char *signaling_stream;
    /* 可选：启动时主动连接（WHEP 拉流 / WHIP 推流）。 */
    const char *whep_client_url;
    const char *whep_client_app;
    const char *whep_client_stream;
    const char *whep_client_token;
    const char *whip_client_url;
    const char *whip_client_app;
    const char *whip_client_stream;
    const char *whip_client_token;
    void *client_ssl_ctx;
} zms_webrtc_server_opts;

typedef struct zms_webrtc_ice_server {
    char url[128];
    char user[64];
    char pass[128];
    int turn;
} zms_webrtc_ice_server;

/** 复制配置的 STUN/TURN 服务器；返回写入条数。 */
ZMS_API size_t zms_webrtc_server_ice_servers(zms_webrtc_ice_server *out, size_t cap);

/**
 * 设置并发 WHEP/WHIP 会话上限（[webrtc] max_sessions）。
 * 必须在服务创建前调用（启动装配阶段，单线程）；0 表示回退默认值。
 * 超过编译期硬上限会被夹到硬上限并告警。
 */
ZMS_API void zms_webrtc_session_set_limit(unsigned limit);
/** @return 当前生效的并发会话上限。 */
ZMS_API size_t zms_webrtc_session_limit(void);

/**
 * 创建 WebRTC 服务并设为全局实例（instance）。poller 必填。
 * 内部会初始化 DTLS/ICE，并按 opts 启动信令与 WHEP/WHIP 主动连接。
 * @return 新服务；opts 或 poller 为空、内存不足返回 NULL。
 */
ZMS_API zms_webrtc_server *zms_webrtc_server_create(const zms_webrtc_server_opts *opts);
/** 销毁服务：停信令、关闭所有主动连接与会话、释放全局实例（若指向自身）。 */
ZMS_API void zms_webrtc_server_destroy(zms_webrtc_server *srv);

/** @return 当前全局 WebRTC 服务实例（借用）；未创建返回 NULL。 */
ZMS_API zms_webrtc_server *zms_webrtc_server_instance(void);
/** 设置全局服务实例（通常由 create 完成，手动设置用于测试/替换）。 */
ZMS_API void zms_webrtc_server_set_instance(zms_webrtc_server *srv);
/** @return 服务绑定主机；未设置或 srv 为 NULL 时返回 "0.0.0.0"。 */
ZMS_API const char *zms_webrtc_server_bind_host(const zms_webrtc_server *srv);
/** @return ICE/SDP 对外通告主机；未设置时返回 "127.0.0.1"。 */
ZMS_API const char *zms_webrtc_server_advertise_host(const zms_webrtc_server *srv);
/**
 * ICE 媒体 UDP 端口范围（[webrtc] port_min/port_max）。
 * @return 1 = 有有效范围；0 = 未配置（调用方回退系统临时端口）
 */
ZMS_API int zms_webrtc_server_port_range(const zms_webrtc_server *srv, uint16_t *out_min,
                                         uint16_t *out_max);
/** ICE 定时器与全部 WebRTC 会话须共享此 poller（线程安全）。 */
ZMS_API zn_poller *zms_webrtc_server_poller(const zms_webrtc_server *srv);
/** 优先 WebRTC 服务 poller 而非 HTTP TCP poller（避免 zice 与会话竞态）。 */
ZMS_API zn_poller *zms_webrtc_server_resolve_poller(struct zms_http_session *hs);

/** 单个 WebRTC 会话的内存/队列快照（运维统计用）。 */
typedef struct zms_webrtc_session_stats {
    char id[64];
    char app[64];
    char stream[128];
    unsigned session_no;
    /** 0=play(WHEP), 1=publish(WHIP) */
    int mode;
    unsigned rtpq_pkts;
    uint64_t rtpq_bytes;
    uint64_t rtpq_bytes_max;
    uint64_t rtpq_dropped_bytes;
    uint64_t rtpq_dropped_pkts;
    uint64_t rtpq_alloc_fail;
    unsigned nack_cache_pkts;
    uint64_t nack_cache_bytes;
    uint64_t nack_hits;
    uint64_t nack_misses;
    uint64_t nack_retransmits;
    uint64_t rtcp_rr_pkts;
    uint32_t rtcp_fraction_lost;
    uint32_t rtcp_jitter;
    int64_t rtcp_rtt_ms;
    /** 已协商的 TWCC 扩展 id；0 表示未协商。 */
    int twcc_ext_id;
    uint64_t twcc_fb_pkts;
    uint64_t twcc_received;
    uint64_t twcc_lost;
    uint32_t twcc_loss_permille;
    uint32_t twcc_loss_ewma;
    uint32_t send_bps;
    uint64_t pace_waits;
    uint32_t adapt_bps;
    int adapt_dropping;
    uint64_t adapt_wait_ms;
    uint64_t adapt_resets;
    uint64_t adapt_dropped_frames;
    uint64_t adapt_dropped_bytes;
    uint64_t pli_count;
    uint64_t pli_suppressed;
    int talk_enabled;
    uint64_t talk_rx_pkts;
    uint64_t talk_rx_bytes;
    uint64_t whip_rx_pkts;
    uint64_t whip_rx_bytes;
    uint64_t whip_rx_dropped;
    uint64_t whip_srtp_fail;
    uint64_t created_ms;
    uint64_t last_rx_ms;
} zms_webrtc_session_stats;

/** 复制当前所有 WebRTC 会话统计；返回写入条数。 */
ZMS_API size_t zms_webrtc_session_stats_all(zms_webrtc_session_stats *out, size_t cap);

/** 对讲音频（浏览器/平台 -> 设备）回调；codec 为 zms_codec_id 数值。 */
typedef void (*zms_webrtc_talk_audio_cb)(const char *app, const char *stream, int codec,
                                         int rate, const uint8_t *data, size_t len, void *user);
/** 注册对讲音频消费者；未注册时 WHEP 会话默认回环给同一 viewer，便于联调。 */
ZMS_API void zms_webrtc_server_set_talk_audio_cb(zms_webrtc_talk_audio_cb cb, void *user);
/** 是否有对讲消费者；无消费者时用于决定是否回环。 */
ZMS_API int zms_webrtc_server_has_talk_audio_cb(void);
/** 投递一帧对讲音频；返回消费回调存在且已调用为 1。 */
ZMS_API int zms_webrtc_server_emit_talk_audio(const char *app, const char *stream, int codec,
                                               int rate, const uint8_t *data, size_t len);

/** 主动连接（WHEP/WHIP client）状态快照。 */
typedef struct zms_webrtc_client_stats {
    char id[32];
    char kind[8];
    char url[512];
    char app[64];
    char stream[128];
    char session_id[64];
    /** 0=HTTP 协商, 1=ICE, 2=DTLS, 3=媒体, -1=失败。 */
    int state;
    int ice_connected;
    int dtls_ready;
    int media_started;
    uint64_t created_ms;
    char error[128];
} zms_webrtc_client_stats;

/**
 * 启动主动连接：kind=whep（拉流写入 app/stream）或 whip（把 app/stream 推给 url）。
 * ssl_ctx 用于 https，可为 NULL。返回 0 表示已排队。
 */
ZMS_API int zms_webrtc_client_launch(const char *kind, const char *url, const char *app,
                                     const char *stream, const char *token, void *ssl_ctx);
/** 复制所有主动连接（WHEP/WHIP client）的统计快照；返回写入条数（最多 cap 条）。 */
ZMS_API size_t zms_webrtc_client_stats_all(zms_webrtc_client_stats *out, size_t cap);
/**
 * 按 id 停止一个主动连接（异步投递到服务 poller 执行，无 poller 时同步）。
 * @return 0 表示已受理；id 为 NULL 或客户端未初始化返回 -1
 */
ZMS_API int zms_webrtc_client_stop(const char *id);
/** 关闭并释放全部主动连接（服务销毁时调用）。 */
ZMS_API void zms_webrtc_client_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SESSION_WEBRTC_SERVER_H */
