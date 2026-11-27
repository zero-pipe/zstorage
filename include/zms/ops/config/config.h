/**
 * @file config.h
 * @brief 配置结构与加载。
 */
#ifndef ZMS_OPS_SERVICE_CONFIG_H
#define ZMS_OPS_SERVICE_CONFIG_H

/**
 * 只读配置加载（INI 子集）。
 * 每个 INI section 对应一个子结构体；zms_config 是所有子结构体的聚合体。
 * 调用方通过 cfg->section.field 访问，如 cfg->rtmp.port、cfg->general.listen_host。
 */
#include "zms/core/auth.h"
#include "zms/core/stream/limits.h"
#include "zms/egress/pacing/egress_pacing.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 尺寸常量 ────────────────────────────────────────────────────────────── */
#define ZMS_CFG_PATH_MAX 512
#define ZMS_CFG_URL_MAX 768
#define ZMS_CFG_HOOK_URL_MAX 512
#define ZMS_CFG_SCHEMA_FILTER_MAX 128

/* ── [general] bufPoolMode 枚举 ──────────────────────────────────────────── */
#define ZMS_BUF_POOL_MAX_PER_BUCKET_DEFAULT 128u
#define ZMS_BUF_POOL_MODE_GLOBAL 0
#define ZMS_BUF_POOL_MODE_PER_POLLER 1
#define ZMS_BUF_POOL_MODE_HYBRID 2

/* ════════════════════════════════════════════════════════════════════════════
 * 子结构体：每个对应一个 INI section
 * ════════════════════════════════════════════════════════════════════════════ */

/** [general] */
typedef struct zms_general_config {
    char log_file[256];
    int log_level;       /**< ZN_LOG_* */
    unsigned log_max_mb; /**< 0 = 不轮转 */
    int log_keep_count;  /**< 默认 5 */
    unsigned poller_threads;
    int stream_none_reader_delay_ms;
    /** 监听地址，默认 0.0.0.0（所有网卡）；可设为具体 IP 限定单网卡 */
    char listen_host[64];
    /** 1 = 按 URL/Host 解析 vhost；0 = 全部归一化为默认 vhost */
    int enable_vhost;
    /** 对外公布 IP（WebRTC ICE candidate / RTSP SDP），对齐 ZLM externIP */
    char extern_ip[64];
    /** 缓冲池 */
    int buf_pool_enable;
    int buf_pool_mode; /**< ZMS_BUF_POOL_MODE_* */
    unsigned buf_pool_max_per_bucket;
    /**
     * 直播 GOP 缓存目标保留段数（压力下丢旧 GOP 的阈值）。
     * 0 = 使用编译默认 ZMS_GOP_QUEUE_TARGET_GOPS；典型 2–8。
     */
    unsigned gop_target_gops;
    /**
     * 直播 GOP 时间窗（秒）。0=关闭，仅按 gop_target_gops/容量裁剪。
     * 与 target_gops 同时生效：超时或超段数都会丢最旧 GOP（至少保留 1 段）。
     */
    unsigned gop_cache_sec;
    /** 每路 GOP 缓存字节上限；0 = 使用编译默认（8MB）。 */
    unsigned gop_max_bytes;
    /** 点播磁盘阻塞线程数，0=I/O poller 上同步执行 */
    unsigned vod_work_threads;
} zms_general_config;

/** [rtmp] */
typedef struct zms_rtmp_config {
    unsigned port;
    unsigned ssl_port; /**< 0=不监听 RTMPS */
} zms_rtmp_config;

/** [rtsp] */
typedef struct zms_rtsp_config {
    unsigned port;
    unsigned ssl_port; /**< 0=不监听 RTSPS */
    char auth_user[64];
    char auth_pass[128];
    int test_reject_tcp_setup;
    /** -1=不限 0=仅 TCP 1=仅 UDP 2=MULTICAST */
    int rtp_transport_type;
    /** RTSP 组播出流：地址/起始端口（0=关闭）/TTL */
    char multicast_addr[64];
    unsigned multicast_port_base;
    int multicast_ttl;
    /** 1 = RTSP 播放低延迟：不按播放时钟 pacing（ZLM lowLatency） */
    int low_latency;
} zms_rtsp_config;

/** [http] api.secret 归入此处（HTTP 层鉴权） */
typedef struct zms_http_config {
    unsigned port;
    unsigned ssl_port; /**< 0=不监听 HTTPS/WSS */
    char api_secret[128];
} zms_http_config;

/** [srt] */
typedef struct zms_srt_config {
    unsigned port;
    int timeout_sec;   /**< peer idle timeout（秒），对应 ZLM timeoutSec */
    int latency_mul;   /**< 延迟倍数，基础 50ms × latencyMul */
    int pkt_buf_size;  /**< 包缓冲大小，换算为字节用于 RCVBUF/SNDBUF */
    char passphrase[128]; /**< 空表示不加密，对应 ZLM passPhrase */
} zms_srt_config;

/** [webrtc] */
typedef struct zms_webrtc_config {
    int enable;
    unsigned port_min;
    unsigned port_max;
    /** 并发 WHEP/WHIP 会话上限；0 = 用默认 64，超过编译期硬上限会被夹住。 */
    unsigned max_sessions;
    /** 可选 STUN/TURN：host:port（支持 stun:/turn: 前缀）。 */
    char stun_server[128];
    char turn_server[128];
    char turn_user[64];
    char turn_pass[128];
    /** 可选：连接到外部 WebSocket 信令（设备作为 master）。 */
    int signaling_enable;
    char signaling_url[256];
    char signaling_room[64];
    char signaling_token[128];
    char signaling_app[64];
    char signaling_stream[128];
    /** 可选：主动连接（WHEP 拉流 / WHIP 推流）。 */
    char whep_client_url[256];
    char whep_client_app[64];
    char whep_client_stream[128];
    char whep_client_token[128];
    char whip_client_url[256];
    char whip_client_app[64];
    char whip_client_stream[128];
    char whip_client_token[128];
} zms_webrtc_config;

/** [protocol] 节；zms_protocol_opts 为其别名（见 protocol_opts.h） */
typedef struct zms_protocol_config {
    int enable_rtmp;
    int enable_rtsp;
    int enable_srt;
    int enable_hls;
    int enable_hls_fmp4;
    /** 1 = 不随推流预建 HLS 切片器，首个 HLS 请求时按需创建（ZLM hls_demand） */
    int hls_demand;
    /** 1 = 不随推流预建 DASH 切片器，首个 DASH 请求时按需创建 */
    int dash_demand;
    /** 非空时把直播 HLS 分片/清单落盘到该目录（{path}/{app}/{stream}/） */
    char hls_save_path[256];
    int enable_audio;
    /** 视频无音轨时补静音 AAC，对应 ZLM add_mute_audio */
    int add_mute_audio;
    /** 断流后保留源/GOP 的时长（毫秒），对应 ZLM continue_push_ms */
    int continue_push_ms;
    /** 0=源时间戳 1=系统时间 2=相对时间（预留） */
    int modify_stamp;
    /** TCP 会话首包超时（秒）；0=关闭。防半开连接/慢速攻击长期占用 fd。 */
    int handshake_timeout_sec;
    /** TCP 会话空闲超时（秒）；0=关闭。 */
    int idle_timeout_sec;
    /** 单服务最大会话数；0=不限。 */
    int max_sessions;
    /** 单 IP 最大会话数；0=不限。 */
    int max_sessions_per_ip;
} zms_protocol_config;

/** [hls] */
typedef struct zms_hls_config {
    float segment_duration_sec;
    unsigned segment_count;
} zms_hls_config;

/** [proxy] */
typedef struct zms_proxy_config {
    char pull[ZMS_CFG_URL_MAX];
    char app[ZMS_APP_MAX];
    /** 空或 auto：按 pull_url 自动生成 proxied/... */
    char stream[ZMS_STREAM_MAX];
    char prefix[64];
    int retry_count; /**< -1=无限 */
    int reconnect_delay_ms;
} zms_proxy_config;

/** [record] */
typedef struct zms_record_config {
    char app[ZMS_APP_MAX];
    char root[ZMS_CFG_PATH_MAX];
    int file_repeat;
    /** 直播 MP4 单文件最长秒数（默认 180）；到期后在关键帧切文件 */
    int mp4_max_second;
    /** 录像根目录总大小上限（MB）；0=不限。超出时删除最旧文件。 */
    int max_total_mb;
    /** 录制前检查磁盘剩余（MB）；低于该值暂停录制并告警。0=不检查。 */
    int free_space_check_mb;
} zms_record_config;

/**
 * [storage] 中心视频云存储（zstorage）参数。
 *
 * 容量与保留天数同时生效，先到者回收：盘写满按最旧录像回收，超过保留天数
 * 也回收。槽位大小固定、总数 = capacity / slot_size，因此磁盘是"一次预分配、
 * 循环使用"，删除旧录像不释放磁盘。
 */
typedef struct zms_storage_config {
    /** 是否启用云存储写入；0 表示只用原有 MP4 录制。 */
    int enable;
    /** 槽位目录（建议挂独立盘）。 */
    char dir[ZMS_CFG_PATH_MAX];
    /** 存储池总容量（GB），默认 2048（2TB）。 */
    int capacity_gb;
    /** 录像保留天数，默认 7。 */
    int retention_days;
    /** 单个槽位文件大小（MB），默认 256。 */
    int slot_size_mb;
    /** 槽位关键帧索引区大小（KB），默认 1024（约 32640 条）。 */
    int index_kb;
    /** 空闲槽位低于该百分比时提前回收，默认 5。 */
    int free_watermark_pct;
    /** 聚合写阈值（KB），默认 512。 */
    int flush_kb;
    /** 最大刷盘延迟（ms），默认 1000。 */
    int flush_ms;
    /** 单槽位最长时长（秒），默认 900（15 分钟）。 */
    int slot_max_second;
    /** 数据区剩余低于该值提前封口（MB），默认 4。 */
    int seal_margin_mb;
    /**
     * 拉流兜底主机（设备->ZMS 的取流地址主机）；调用方只给 channel_key 不给 URL
     * 时按 "rtsp://<pull_host>:<pull_port>/live/<channel_key>" 拼装。
     */
    char pull_host[128];
    /** 拉流兜底 RTSP 端口，默认 554。 */
    int pull_port;
} zms_storage_config;

/**
 * [play] 包装 zms_egress_pacing，保持 pacing 成员名一致。
 * 调用方可直接 &cfg->play.pacing 传给 zms_egress_pacing_init()。
 */
typedef struct zms_play_config {
    zms_egress_pacing pacing;
} zms_play_config;

/**
 * [auth] 推流/播放 token 验签。
 *
 * 业务鉴权在上游 web 后台完成，由它签发带时效的 token；ZMS 只做本地验签，
 * 不回调 hook、不查库。验签实现由上层通过 zms_auth_set_verifier() 注册。
 */
typedef struct zms_auth_config {
    /** 0=不验签（默认，全放行）；1=验签（未注册实现时 fail-closed） */
    int enable;
    /** token 参数名（URL query 中的键），默认 "token" */
    char token_param[ZMS_AUTH_PARAM_NAME_MAX];
    /** 预留给验签实现使用的签名密钥 */
    char secret[128];
} zms_auth_config;

/** [hook] 事件通知（只做通知，不再承担鉴权） */
typedef struct zms_hook_config {
    int enable;
    float timeout_sec;
    int retry_count;
    float retry_delay_sec;
    char media_server_id[64];
    char on_stream_changed[ZMS_CFG_HOOK_URL_MAX];
    char on_stream_none_reader[ZMS_CFG_HOOK_URL_MAX];
    char on_play_stop[ZMS_CFG_HOOK_URL_MAX];
    char on_server_started[ZMS_CFG_HOOK_URL_MAX];
    /** 周期心跳/续注册地址（留空=不发）。上报端口与协议能力，供平台维持节点在线与能力。 */
    char on_server_keepalive[ZMS_CFG_HOOK_URL_MAX];
    /** 心跳间隔（秒）；<=0 用默认 30。 */
    int keepalive_interval_sec;
    /** 直播 MP4 切片落盘通知（对齐 ZLM on_record_mp4） */
    char on_record_mp4[ZMS_CFG_HOOK_URL_MAX];
    /* ── 云存储节点自注册（zstorage 作为独立存储节点接平台，与取流 ZMS 节点分离）──
     * 留空则不发；载荷带容量/已用，平台落独立表 zws_storage_node。 */
    char on_storage_started[ZMS_CFG_HOOK_URL_MAX];
    char on_storage_keepalive[ZMS_CFG_HOOK_URL_MAX];
    /** PS 槽位封口完成通知（平台据此登记 zws_cloud_record） */
    char on_record_ps[ZMS_CFG_HOOK_URL_MAX];
    /** 留空表示不过滤；rtsp/rtmp */
    char stream_changed_schemas[ZMS_CFG_SCHEMA_FILTER_MAX];
} zms_hook_config;

/** [ssl] */
typedef struct zms_ssl_config {
    /** 服务端证书/私钥（PEM）；两者都非空且对应 sslport>0 时启用 TLS 监听 */
    char cert_file[ZMS_CFG_PATH_MAX];
    char key_file[ZMS_CFG_PATH_MAX];
    char ca_file[ZMS_CFG_PATH_MAX];
    int verify_peer;
    char client_cert[ZMS_CFG_PATH_MAX];
    char client_key[ZMS_CFG_PATH_MAX];
} zms_ssl_config;

/** [onvif] 客户端（接入 IPC）默认参数；API 始终可用，enable 仅作为默认开关。 */
typedef struct zms_onvif_config {
    int enable;
    /** 1 = 同时作为 ONVIF 设备端对外提供 /onvif/* 服务 */
    int server_enable;
    char server_name[64];
    /** 设备 UUID（urn:uuid:...）；空则自动生成 */
    char server_uuid[80];
    /** 设备映射默认值：vhost 空=默认 vhost；app 空="onvif" */
    char default_vhost[64];
    char default_app[ZMS_APP_MAX];
    /** 设备默认凭据（API 未传时使用） */
    char auth_user[64];
    char auth_pass[128];
    int timeout_ms;
    int retry_count; /**< -1=无限 */
    int reconnect_delay_ms;
} zms_onvif_config;

/** [gb28181] 设备端（ZMS 作为 IPC 注册到上级平台）。 */
typedef struct zms_gb28181_config {
    int enable;
    char platform_id[32];
    char platform_domain[32];
    char platform_ip[64];
    unsigned platform_port;
    char device_id[32];
    char device_name[128];
    char manufacturer[64];
    char model[64];
    char firmware[32];
    char username[64];
    char password[128];
    /** 本机 SIP 对外 IP；空 = 按平台地址自动探测 */
    char local_ip[64];
    unsigned local_port;
    /** SIP 传输：udp（默认）/ tcp / tls（设备作为客户端连平台） */
    char sip_transport[8];
    /** client（默认，设备连平台）/ server（平台连设备；tcp/tls） */
    char sip_mode[8];
    char tls_ca[ZMS_CFG_PATH_MAX];
    char tls_cert[ZMS_CFG_PATH_MAX];
    char tls_key[ZMS_CFG_PATH_MAX];
    int tls_verify_peer;
    int register_expires;
    int keepalive_interval;
    /** 通道映射：`<id>:<vhost>/<app>/<stream>`，分号分隔 */
    char channels[1024];
    /** 录像根目录；空 = [record] root */
    char record_root[ZMS_CFG_PATH_MAX];
} zms_gb28181_config;

/** [gb28181_platform] 平台端：接受下级 GB28181 设备注册与查询。 */
typedef struct zms_gb28181_platform_config {
    int enable;
    char id[32];
    char domain[32];
    char password[128];
    char advertise_ip[64];
    unsigned listen_port;
    /** udp / tcp / tls（tcp/tls 为服务端，设备连入） */
    char transport[8];
    char tls_cert[ZMS_CFG_PATH_MAX];
    char tls_key[ZMS_CFG_PATH_MAX];
    char tls_ca[ZMS_CFG_PATH_MAX];
    int tls_verify_peer;
    int keepalive_timeout_sec;
    /** 平台 INVITE 取流写入的 vhost/app（stream=通道 ID，可被 API 覆盖） */
    char media_vhost[64];
    char media_app[64];
} zms_gb28181_platform_config;

/* ════════════════════════════════════════════════════════════════════════════
 * 顶层聚合：zms_config
 * ════════════════════════════════════════════════════════════════════════════ */
typedef struct zms_config {
    zms_general_config general;
    zms_rtmp_config rtmp;
    zms_rtsp_config rtsp;
    zms_http_config http;
    zms_srt_config srt;
    zms_webrtc_config webrtc;
    zms_protocol_config protocol;
    zms_hls_config hls;
    zms_proxy_config proxy;
    zms_record_config record;
    zms_storage_config storage;
    zms_play_config play;
    zms_auth_config auth;
    zms_hook_config hook;
    zms_ssl_config ssl;
    zms_onvif_config onvif;
    zms_gb28181_config gb28181;
    zms_gb28181_platform_config gb28181_platform;
} zms_config;

/* ── API ─────────────────────────────────────────────────────────────────── */
/** 用内置默认值填充 cfg（各 section 的端口、超时、路径等）。 */
ZMS_API void zms_config_default(zms_config *cfg);
/**
 * 从 INI 文件加载配置到 cfg（INI 子集；缺省 section 先用默认值）。
 * 未知键仅告警，不阻断加载。
 * @return ZN_OK 成功；打不开文件返回 IO；参数非法返回 INVALID。
 */
ZMS_API zn_err_t zms_config_load_ini(zms_config *cfg, const char *path);

/**
 * 查询某个 [section] key 是否为受支持配置项（含驼峰别名）。
 * 用于配置模板一致性测试与外部配置检查工具。
 */
ZMS_API int zms_config_key_supported(const char *section, const char *key);

/**
 * 遍历全部受支持配置项（主键名，不含别名），按 section 顺序回调。
 * @param alias 该键的兼容别名（驼峰形式），无则为空串。
 * 用于生成/检查配置模板，避免文档与代码脱节。
 */
ZMS_API void zms_config_foreach_key(void (*fn)(const char *section, const char *key,
                                               const char *alias, void *user),
                                    void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_OPS_SERVICE_CONFIG_H */
