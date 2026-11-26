/**
 * @file gb28181_internal.h
 * @brief GB28181 设备端模块内部共享定义（SIP / MANSCDP / 会话 / 媒体）。
 */
#ifndef ZMS_SRC_DEVICE_GB28181_INTERNAL_H
#define ZMS_SRC_DEVICE_GB28181_INTERNAL_H

#include "zms/device/gb28181/gb28181.h"
#include "zms/device/core/device_core.h"
#include "device/core/sip_transport.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/hub.h"
#include "zms/media/codec/codec_id.h"
#include "zms/message/msg.h"
#include "zms/message/xml.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tcp_server.h"
#include "zn/net/udp_server.h"
#include "zn/poller/poller.h"
#include "zn/poller/poller_pool.h"
#include "zb/os/sync.h"
#include "zn/util/timer.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_SIP_LINE_MAX 512
#define GB_SIP_HDR_MAX 32
#define GB_SIP_BODY_MAX 32768
/** SIP 报文最大长度：MANSCDP body 上限 + 头部余量 */
#define GB_SIP_MSG_MAX (GB_SIP_BODY_MAX + 4096)
#define GB_SIP_MAX_SESSIONS 16u

/* ── 简易字符串拼接缓冲 ─────────────────────────────────────────────────── */

typedef struct gb_sb {
    char *buf;
    size_t cap;
    size_t len;
} gb_sb;

void gb_sb_init(gb_sb *w, char *buf, size_t cap);
void gb_sb_puts(gb_sb *w, const char *s);
void gb_sb_printf(gb_sb *w, const char *fmt, ...);
void gb_sb_xml_esc(gb_sb *w, const char *s);

/* ── SIP 消息 ───────────────────────────────────────────────────────────── */

typedef struct gb_sip_hdr {
    char name[40];
    char value[GB_SIP_LINE_MAX];
} gb_sip_header;

typedef struct gb_sip_msg {
    int is_response;
    char method[16];
    int status;
    char reason[64];
    char uri[256];
    gb_sip_header hdrs[GB_SIP_HDR_MAX];
    int hdr_count;
    char body[GB_SIP_BODY_MAX];
    size_t body_len;
    char peer_ip[64];
    uint16_t peer_port;
} gb_sip_msg;

/** 解析一条完整 SIP 消息（请求或应答）。@return 0 成功。 */
int gb_sip_parse(const void *data, size_t len, const char *peer_ip, uint16_t peer_port,
                 gb_sip_msg *out);

const char *gb_sip_hdr(const gb_sip_msg *m, const char *name);

/** 取 URI 的 user 部分（sip:user@host → user）。 */
void gb_sip_uri_user(const char *uri, char *out, size_t cap);
/** 取 URI 的 host 部分（可带端口；不带 "sip:"）。 */
void gb_sip_uri_host(const char *uri, char *out, size_t cap);

/** 取 SIP 头参数值，如 From: <...>;tag=abc → tag。 */
void gb_sip_param(const char *value, const char *name, char *out, size_t cap);

typedef struct gb_sip_req_spec {
    const char *method;
    const char *uri;
    const char *from_user;
    const char *from_host;
    const char *from_tag;
    const char *to_user;
    const char *to_host;
    const char *call_id;
    unsigned cseq;
    const char *local_ip;
    unsigned local_port;
    const char *transport; /**< "UDP" / "TCP"（默认 UDP） */
    const char *contact_user;
    int has_expires;
    int expires;
    const char *subject;
    const char *authorization;
    const char *content_type;
    const char *body;
    size_t body_len;
} gb_sip_req_spec;

/**
 * SIP 发送缓冲（GB_SIP_MSG_MAX 字节）。
 * 一张 36KB 的报文放在栈上会压垮小栈线程，统一从堆上取；调用方负责 free。
 */
char *gb_sip_tx_alloc(void);

int gb_sip_build_request(char *out, size_t cap, const gb_sip_req_spec *spec);

int gb_sip_build_response(char *out, size_t cap, const gb_sip_msg *req, int status,
                          const char *reason, const char *local_ip, unsigned local_port,
                          const char *to_tag, const char *contact_user,
                          const char *content_type, const void *body, size_t body_len);
int gb_sip_build_response_ex(char *out, size_t cap, const gb_sip_msg *req, int status,
                             const char *reason, const char *local_ip, unsigned local_port,
                             const char *to_tag, const char *contact_user,
                             const char *content_type, const void *body, size_t body_len,
                             const char *extra_headers);

void gb_rand_hex(char *out, size_t digits);

/* ── SIP Digest ─────────────────────────────────────────────────────────── */

typedef struct gb_digest_challenge {
    char realm[128];
    char nonce[128];
    char opaque[128];
    char qop[32];
    char algorithm[24];
} gb_digest_challenge;

int gb_digest_parse_challenge(const char *www_auth, gb_digest_challenge *out);
int gb_digest_build_authorization(const char *user, const char *pass, const char *method,
                                  const char *uri, const gb_digest_challenge *ch, int nc,
                                  char *out, size_t cap);
/** 平台侧校验 Authorization；成功返回 1。 */
int gb_digest_verify(const char *auth_header, const char *method, const char *uri,
                     const char *realm, const char *nonce, const char *user, const char *pass);

/* ── MANSCDP XML ────────────────────────────────────────────────────────── */

/**
 * 一条 MANSCDP 消息。
 *
 * msg 是统一外壳（body 视图），root/cmd_type/sn/... 是常用字段的解析缓存，
 * 只为既有调用点保留；新增字段请用 gb_manscdp_get() 按标签名取，不要加成员。
 */
typedef struct gb_manscdp_msg {
    zms_msg msg;
    char root[16];     /**< Notify / Response / Query / Control */
    char cmd_type[32]; /**< Keepalive / Catalog / DeviceInfo / ... */
    char sn[32];
    char device_id[ZMS_GB28181_ID_MAX];
    char result[16];
    char ptz_cmd[256];
    char record_cmd[32];
    char guard_cmd[32];
    char start_time[40];
    char end_time[40];
    char type[16];
    int secrecy;
    int sum_num;
} gb_manscdp_msg;

int gb_manscdp_parse(const char *body, size_t len, gb_manscdp_msg *out);

/**
 * 按 XML 标签名取 MANSCDP 字段（新增字段用这个，不必扩展结构体）。
 * @return 0 找到；-1 未找到/超长
 */
int gb_manscdp_get(const gb_manscdp_msg *q, const char *tag, char *out, size_t cap);

int gb_manscdp_build_keepalive(char *out, size_t cap, const char *device_id, const char *sn);
int gb_manscdp_build_device_info(char *out, size_t cap, const char *sn, const char *device_id,
                                 const char *device_name, const char *manufacturer,
                                 const char *model, const char *firmware, int channel_count);
int gb_manscdp_build_device_status(char *out, size_t cap, const char *sn, const char *device_id,
                                   int online);
int gb_manscdp_build_device_control_resp(char *out, size_t cap, const char *sn,
                                         const char *device_id, const char *result);

typedef struct gb_catalog_item {
    const char *device_id;
    const char *name;
    const char *manufacturer;
    const char *model;
    const char *parent_id;
    int online;
    int ptz_type; /**< 0=不支持云台 */
} gb_catalog_item;

int gb_manscdp_build_catalog(char *out, size_t cap, const char *sn, const char *device_id,
                             const gb_catalog_item *items, size_t count);

typedef struct gb_record_item {
    const char *device_id;
    const char *name;
    const char *file_path;
    const char *start_time; /**< YYYY-MM-DDTHH:MM:SS */
    const char *end_time;
    const char *type;
} gb_record_item;

int gb_manscdp_build_record_info(char *out, size_t cap, const char *sn, const char *device_id,
                                 const gb_record_item *items, size_t count, int sum_num);

int gb_manscdp_build_alarm(char *out, size_t cap, const char *sn, const char *device_id,
                           int priority, int method, const char *description);

/** 本地时间（秒）→ "YYYY-MM-DDTHH:MM:SS" */
void gb_format_time_iso(time_t t, char *out, size_t cap);

/* ── 设备 / 通道模型 ────────────────────────────────────────────────────── */

typedef zms_device_channel_info gb_channel;

typedef struct gb_media_session gb_media_session;
typedef struct gb_playback gb_playback;

struct gb28181_ctx {
    int running;
    int registered;

    char platform_id[ZMS_GB28181_ID_MAX];
    char platform_domain[ZMS_GB28181_ID_MAX];
    char platform_ip[64];
    unsigned platform_port;

    char device_id[ZMS_GB28181_ID_MAX];
    char device_name[ZMS_GB28181_NAME_MAX];
    char manufacturer[64];
    char model[64];
    char firmware[32];
    char username[64];
    char password[128];

    char local_ip[64];
    unsigned local_port;
    int register_expires;
    int keepalive_interval;

    char record_root[512];

    zn_poller_pool *pool;
    zn_poller *poller;
    zms_sip_transport *sip;
    int sip_transport_mode; /**< ZMS_SIP_TRANSPORT_* */
    int sip_server_mode;    /**< 1 = 平台连入 */
    zn_timer *tick;

    /* REGISTER 事务 */
    int reg_state; /**< 0=idle 1=wait_ok 2=registered 3=backoff */
    int reg_auth;  /**< 已带 Authorization 重试次数 */
    int reg_retry;
    unsigned cseq;
    char call_id[96];
    char from_tag[64];
    char nonce[128];
    char realm[128];
    uint64_t next_reg_ms;
    uint64_t next_keepalive_ms;
    uint64_t tx_deadline_ms;
    char tx_buf[4096];
    size_t tx_len;
    int tx_pending;

    uint32_t sn;

    zb_mutex *mtx;
    gb_media_session *sessions;

    char last_peer_ip[64];
    uint16_t last_peer_port;
};

extern struct gb28181_ctx *gb_ctx(void);

int gb_transport_send(const char *ip, uint16_t port, const char *data, size_t len);

/* ── 信令处理（gb_device.c / gb_invite.c） ─────────────────────────────── */

void gb_sip_dispatch(const gb_sip_msg *m);
void gb_register_tick(uint64_t now_ms);
void gb_register_start(void);
void gb_register_stop(void);

int gb_send_manscdp(const char *ip, uint16_t port, const char *body);

void gb_send_response(const gb_sip_msg *m, int status, const char *reason, const char *to_tag,
                      const char *contact_user, const char *content_type, const void *body,
                      size_t body_len);

void gb_handle_query(const gb_sip_msg *m, const gb_manscdp_msg *q);
void gb_handle_control(const gb_sip_msg *m, const gb_manscdp_msg *q);

/* INVITE / ACK / BYE（gb_invite.c） */
void gb_invite_handle(const gb_sip_msg *m);
void gb_invite_handle_ack(const gb_sip_msg *m);
void gb_invite_handle_bye(const gb_sip_msg *m);
void gb_invite_handle_info(const gb_sip_msg *m);

int gb_channel_get(const char *id, gb_channel *out);
zms_media_source *gb_channel_source(const gb_channel *ch);
int gb_channel_online(const gb_channel *ch);
int gb_channels_load(const char *spec);

/* 录像：RecordInfo / 回放（gb_playback.c） */
#define GB_RECORD_MAX ZMS_DEVICE_RECORD_MAX

typedef zms_device_record_entry gb_record_ent;
typedef zms_device_record_list gb_record_list;

gb_record_list *gb_record_query(const char *channel_id, const char *start_time,
                                const char *end_time);
gb_playback *gb_playback_open(const char *channel_id, const char *start_time,
                              const char *end_time, zn_poller *poller);
void gb_playback_close(gb_playback *p);
/** 底层 VOD source（取 codec/config）。 */
zms_media_source *gb_playback_source(gb_playback *p);
const uint8_t *gb_playback_video_config(gb_playback *p, size_t *len);
const uint8_t *gb_playback_audio_config(gb_playback *p, size_t *len);
/** 回放线程步进：返回 1=有帧，0=暂无，-1=结束/错误。slot->data 有效期到下次调用。 */
int gb_playback_next(gb_playback *p, zms_gop_slot *slot, int timeout_ms);

/* ── 媒体会话（gb_media.c） ─────────────────────────────────────────────── */

typedef enum gb_media_transport {
    GB_MEDIA_UDP = 0,
    GB_MEDIA_TCP_PASSIVE = 1, /**< 本端 listen，平台连入 */
    GB_MEDIA_TCP_ACTIVE = 2,  /**< 本端连平台 */
} gb_media_transport;

struct gb_media_session {
    struct gb_media_session *next;
    char call_id[96];
    char channel_id[ZMS_GB28181_ID_MAX];
    char platform_id[ZMS_GB28181_ID_MAX];
    uint32_t ssrc;
    uint8_t payload_type;
    gb_media_transport transport;

    char remote_ip[64];
    uint16_t remote_port; /**< UDP 对端 / TCP-ACTIVE 平台媒体端口 */

    int media_fd;       /**< UDP 发送 socket 或 TCP 监听 socket */
    uint16_t local_port;/**< answer SDP 中的端口 */
    int media_connected;/**< TCP 已建立 */

    int started;
    int stop;
    /** 引用计数：持有指针者 +1，最后一个释放者负责销毁（跨线程安全）。 */
    int32_t refs;

    zms_media_source *source;
    zms_media_subscriber reader;
    int reader_valid;

    void *ps_mux;  /**< zms_zm_ps_muxer* */
    void *rtp_ps;  /**< zms_zm_rtp_ps* */
    int video_stream;
    int audio_stream;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;

    gb_playback *playback;
    int is_playback;
    char play_start[32];
    char play_end[32];

    uint8_t *au_buf;
    size_t au_cap;
    uint8_t *aud_buf;
    size_t aud_cap;
    void *aac_cfg; /**< zms_aac_config*（AAC → ADTS） */
    void *params;  /**< zms_codec_param_sets* */
    uint32_t cur_rtp_ts;
    uint32_t last_rtp_ts;
    uint64_t first_wall_ms;
    uint32_t first_dts_ms;
    int have_pace_base;

    int64_t base_pts_90k;
    uint64_t base_wall_ms;
    uint64_t last_dts_ms;
    int have_base;

    uint64_t bytes_sent;
    uint64_t packets_sent;

    /* ── poller 驱动（无专用线程）────────────────────────────────────── */
    zn_poller *poller;
    /** 等源上线的重试定时器（一次性）。 */
    zn_poller_timer *start_timer;
    /** 发送节拍定时器（周期）。 */
    zn_poller_timer *tick_timer;
    /** media_fd 当前注册的事件（0=未注册）。 */
    unsigned fd_events;
    int connecting;      /**< TCP-ACTIVE：等待 connect 完成 */
    int accepting;       /**< TCP-PASSIVE：等待平台连入 */
    int video_started;   /**< 已从 IDR 起播 */
    uint64_t source_deadline_ms;
    uint64_t next_due_ms;    /**< 回放节拍：下一帧最早发送时刻 */
    int playback_reading;    /**< 有一个异步读在途 */
    /** TCP 发送缓冲（2 字节长度前缀 + RTP），非阻塞散写用。 */
    uint8_t *tx_buf;
    size_t tx_len;
    size_t tx_off;
    size_t tx_cap;
};

/** 创建媒体会话（打开套接字、建立 answer SDP 所需端口）。 */
gb_media_session *gb_media_open(const char *call_id, const char *channel_id,
                                const char *platform_id, uint32_t ssrc, uint8_t pt,
                                gb_media_transport transport, const char *remote_ip,
                                uint16_t remote_port, int *out_local_port);

/** 收到 ACK 后启动发送（在 gb poller 上以定时器节拍驱动，不再起线程）。 */
int gb_media_start(gb_media_session *s, const gb_channel *ch);

/** 测试注入：无 GB 上下文时指定 poller。 */
void gb_media_set_poller_for_test(zn_poller *p);

/** 按 Call-ID 停止并销毁会话；返回 1 表示找到。 */
int gb_media_stop_call(const char *call_id);

/** 查找会话并取一份引用；用完必须 gb_media_session_unref()。 */
gb_media_session *gb_media_find_call(const char *call_id);
/** 释放一份引用；最后一份负责销毁（必要时 join 已结束的线程）。 */
void gb_media_session_unref(gb_media_session *s);

void gb_media_stop_all(void);
int gb_media_session_count(void);

/* 回放会话需要媒体会话的发送回调。 */

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_DEVICE_GB28181_INTERNAL_H */
