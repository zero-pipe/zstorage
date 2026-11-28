/**
 * @file session_internal.h
 * @brief RTSP 会话私有结构。
 */
#ifndef ZCS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H
#define ZCS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H

#include "zcs/media/container/demux_pipeline.h"
#include "zcs/egress/pipeline/egress_pipeline.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/core/auth.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/media/media_limits.h"
#include "zcs/core/stream/hub.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/protocol/rtsp/parser.h"
#include "zcs/egress/rtp/rtp_play_pump.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include "zcs/protocol/rtsp/splitter.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zcs/protocol/rtsp/sdp.h"
#include "zcs/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/net/udp_client.h"
#include "zn/poller/poller.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；驱动它的 .c 需 include vod_play_lane.h。 */
typedef struct zcs_vod_play_lane zcs_vod_play_lane;

typedef enum zcs_rtsp_session_mode {
    ZCS_RTSP_SESSION_MODE_IDLE,
    ZCS_RTSP_SESSION_MODE_PLAY,
    ZCS_RTSP_SESSION_MODE_RECORD,
} zcs_rtsp_session_mode;

typedef struct zcs_rtsp_session zcs_rtsp_session;

typedef struct zcs_rtsp_destroy_job {
    zcs_rtsp_session *rs;
    unsigned token;
} zcs_rtsp_destroy_job;

typedef struct zcs_rtsp_session_udp_track {
    zn_udp_client *rtp;
    zn_udp_client *rtcp;
    uint16_t local_rtp_port;
    uint16_t local_rtcp_port;
    uint16_t peer_rtp_port;
    uint16_t peer_rtcp_port;
    struct {
        zcs_rtsp_session *rs;
        int track_idx;
    } rtp_cb;
} zcs_rtsp_session_udp_track;

struct zcs_rtsp_session {
    unsigned session_no;
    zn_tcp_session *tcp;
    zcs_rtsp_splitter *splitter;
    /** ── 推流侧（RECORD，GB/RTSP 推流）──────────────────────────────── */
    struct {
        zcs_sdp_session publish_sdp;
        int publish_track_setup[ZCS_SDP_TRACK_MAX];
        zcs_live_ingest *ingress;
        void *zm_rtp; /**< zcs_zm_rtp_ingest（固定） */
        int logged_record;
        int logged_record_h264;
        unsigned record_rtp_count;
        zcs_codec_id audio_codec;
        int audio_rate;
        int audio_channels;
        uint32_t video_clock_hz;
        uint32_t audio_clock_hz;
    } pb;

    /** ── 播放侧（PLAY）──────────────────────────────────────────────── */
    struct {
        zcs_egress_source src;
        zcs_gop_reader *gop_reader;
        zcs_vod_buffer_reader *vod_reader;
        zcs_vod_play_lane *vod_lane;
        zcs_rtp_muxer *rtp_muxer;
        zcs_egress_pipeline *egress_pipe;
        zcs_rtp_play_sender *sender;
        int rtcp_tick;
        int boot_sent;
        int rtcp_boot_sent;
        int rtcp_video_sr_sent;
        int rtcp_audio_sr_sent;
        int paused;
        uint64_t seek_ms;
        double scale;
        int live_catchup;
        uint64_t lag_resync_ms;
        int config_pending;
        int audio_setup;
        int video_setup;
        int reader_attached;
        uint32_t video_rtp_ssrc;
        uint32_t audio_rtp_ssrc;
        unsigned udp_video_send_ok;
        unsigned udp_video_send_fail;
        uint8_t *interleaved_buf;
    } pl;

    /* 信令/会话标识 */
    char session_id[32];
    int auth_ok;
    char auth_nonce[64];
    char content_base[512];
    char vhost[ZCS_VHOST_MAX];
    int low_latency;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    unsigned cseq;
    zcs_rtsp_session_mode mode;
    /* RTP 通道号 */
    uint8_t video_rtp_ch;
    uint8_t video_rtcp_ch;
    uint8_t audio_rtp_ch;
    uint8_t audio_rtcp_ch;
    int close_pending;
    int destroy_scheduled;
    zcs_media_source *source;
    char params[ZCS_AUTH_PARAMS_MAX];
    zcs_codec_id audio_codec;
    int audio_rate;
    int audio_channels;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    int play_audio_setup;
    int play_video_setup;
    int play_reader_attached;
    uint64_t
        play_start_ms; /**< 播放开始 zn_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    unsigned destroy_token;
    zcs_rtp_play_sender *play_sender;
    zn_poller *poller;
    zcs_rtsp_rtp_mode rtp_mode;
    char peer_ip[64];
    /** SDP o= / c= 对外公告地址（来自 zcs_rtsp_server_opts.advertise_host） */
    char advertise_host[64];
    zcs_rtsp_session_udp_track udp_tracks[ZCS_SDP_TRACK_MAX];
    void *zm_rtp; /**< zcs_zm_rtp_ingest（固定） */
    unsigned play_udp_video_send_ok;
    unsigned play_udp_video_send_fail;
    /** RTSP 信令响应缓冲（buf_pool，按需扩到 ZCS_MEDIA_IO_BUF_SIZE） */
    uint8_t *resp_buf;
    size_t resp_cap;
    uint8_t *play_interleaved_buf;
    size_t play_interleaved_cap;
};

typedef struct zcs_rtsp_vod_play_ctx {
    zcs_rtsp_session *rs;
    zcs_media_source *src;
    uint64_t seek_ms;
    zcs_vod_play_lane *lane;
    int is_replay;
    int was_paused;
    int did_seek;
    int range_parsed;
    int range_now;
    double scale;
    char range_req[160];
} zcs_rtsp_vod_play_ctx;

typedef void (*zcs_rtsp_method_fn)(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);

void zcs_rtsp_session_send_resp(zcs_rtsp_session *rs, int code, const char *reason,
                                const char *extra, const char *body, size_t body_len);
void zcs_rtsp_session_parse_url(const char *url, char *app, char *stream);
/** 记录请求 URL 的查询串（鉴权参数），供 RECORD/PLAY 验签使用。 */
void zcs_rtsp_session_capture_params(zcs_rtsp_session *rs, const char *url);
void zcs_rtsp_url_vhost(const char *url, char *out, size_t cap);

void zcs_rtsp_multicast_configure(const char *addr, unsigned port_base, int ttl);
int zcs_rtsp_multicast_enabled(void);
const char *zcs_rtsp_multicast_addr(void);
unsigned zcs_rtsp_multicast_port_base(void);
int zcs_rtsp_multicast_ttl(void);
int zcs_rtsp_session_setup_multicast_play(zcs_rtsp_session *rs, int track_idx, char *extra,
                                          size_t extra_cap);
int zcs_rtsp_session_match(zcs_rtsp_session *rs, const char *session_hdr);
int zcs_rtsp_session_setup_track_id(const char *url, const zcs_sdp_session *sdp);
int zcs_rtsp_session_record_any_track_setup(const zcs_rtsp_session *rs);
int zcs_rtsp_session_record_track_by_channel(const zcs_rtsp_session *rs, uint8_t channel);
void zcs_rtsp_session_load_audio_params(zcs_rtsp_session *rs);
/** VOD 总时长（ms）：publisher 元数据 + 文件 probe 回退 */
uint64_t zcs_rtsp_session_vod_duration_ms(const zcs_rtsp_session *rs);
/** VOD 当前播放位置（ms）：优先已发送视频 RTP 时间 */
uint64_t zcs_rtsp_session_vod_play_position_ms(const zcs_rtsp_session *rs);
void zcs_rtsp_session_build_sdp(zcs_rtsp_session *rs, char *out, size_t cap);
void zcs_rtsp_session_send_rtp_interleaved(zcs_rtsp_session *rs, uint8_t channel,
                                           const uint8_t *rtp, size_t rtp_len);
void zcs_rtsp_session_send_rtcp_srs(zcs_rtsp_session *rs);
void zcs_rtsp_session_write_interleaved(void *user, uint8_t channel, const uint8_t *payload,
                                        size_t len);
void zcs_rtsp_session_publisher_kick(void *ctx, int force);

void zcs_rtsp_session_handle_options(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_announce(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_describe(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_setup(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_record(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_play(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_pause(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_get_parameter(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
void zcs_rtsp_session_handle_teardown(zcs_rtsp_session *rs, const zcs_rtsp_message *msg);

void zcs_rtsp_session_on_message(const zcs_rtsp_message *msg, void *user);
void zcs_rtsp_session_on_rtp(uint8_t channel, const uint8_t *data, size_t len, void *user);
void zcs_rtsp_session_play_on_rtcp(zcs_rtsp_session *rs, const uint8_t *data, size_t len);
void zcs_rtsp_session_record_payload_teardown(zcs_rtsp_session *rs);
void zcs_rtsp_session_record_input_rtp_raw(zcs_rtsp_session *rs, int track_idx, const uint8_t *data,
                                           size_t len);
int zcs_rtsp_session_setup_udp_record(zcs_rtsp_session *rs, int track_idx,
                                      const char *transport_hdr, char *extra, size_t extra_cap);
int zcs_rtsp_session_setup_udp_play(zcs_rtsp_session *rs, int track_idx, const char *transport_hdr,
                                    char *extra, size_t extra_cap);
void zcs_rtsp_session_udp_teardown(zcs_rtsp_session *rs);
void zcs_rtsp_session_send_media(zcs_rtsp_session *rs, uint8_t channel, const uint8_t *payload,
                                 size_t len);
void zcs_rtsp_session_play_tick(zcs_rtsp_session *rs);
void zcs_rtsp_session_play_kick(zcs_rtsp_session *rs);
zn_err_t zcs_rtsp_session_play_vod_lane_attach(zcs_rtsp_session *rs, zcs_media_source *src,
                                                uint64_t seek_ms);
void zcs_rtsp_session_egress_close(zcs_rtsp_session *rs);
void zcs_rtsp_session_play_mux_destroy(zcs_rtsp_session *rs);
void zcs_rtsp_session_fill_play_ctx(zcs_rtsp_session *rs, zcs_rtp_play_run_ctx *ctx);
zcs_rtsp_play_rtp_info_args zcs_rtsp_session_rtp_info_args(zcs_rtsp_session *rs, uint32_t anchor_ms,
                                                           int vod_linear);
void zcs_rtsp_session_play_mux_create(zcs_rtsp_session *rs);
void zcs_rtsp_session_play_try_rtcp_boot(zcs_rtsp_session *rs);
void zcs_rtsp_session_send_rtcp_sr_tracks(zcs_rtsp_session *rs, int send_video, int send_audio);
void zcs_rtsp_session_vod_play_continue(zcs_rtsp_session *rs, int is_replay, int was_paused,
                                        int did_seek, uint64_t seek_ms, double scale,
                                        const char *range_req, int range_parsed, int range_now);
int zcs_rtsp_session_vod_open_try_async(zcs_rtsp_vod_play_ctx *job);

void zcs_rtsp_session_teardown(zcs_rtsp_session *rs);
void zcs_rtsp_session_schedule_destroy(zcs_rtsp_session *rs, zn_tcp_session *session);

#endif /* ZCS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H */
