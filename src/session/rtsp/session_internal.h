/**
 * @file session_internal.h
 * @brief RTSP 会话私有结构。
 */
#ifndef ZMS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H
#define ZMS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H

#include "zms/media/container/demux_pipeline.h"
#include "zms/egress/pipeline/egress_pipeline.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/core/auth.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/buffer.h"
#include "zms/media/media_limits.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/source/egress_source.h"
#include "zms/protocol/rtsp/parser.h"
#include "zms/egress/rtp/rtp_play_pump.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/protocol/rtsp/splitter.h"
#include "zms/protocol/rtsp/transport.h"
#include "zms/protocol/rtsp/sdp.h"
#include "zms/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/net/udp_client.h"
#include "zn/poller/poller.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；驱动它的 .c 需 include vod_play_lane.h。 */
typedef struct zms_vod_play_lane zms_vod_play_lane;

typedef enum zms_rtsp_session_mode {
    ZMS_RTSP_SESSION_MODE_IDLE,
    ZMS_RTSP_SESSION_MODE_PLAY,
    ZMS_RTSP_SESSION_MODE_RECORD,
} zms_rtsp_session_mode;

typedef struct zms_rtsp_session zms_rtsp_session;

typedef struct zms_rtsp_destroy_job {
    zms_rtsp_session *rs;
    unsigned token;
} zms_rtsp_destroy_job;

typedef struct zms_rtsp_session_udp_track {
    zn_udp_client *rtp;
    zn_udp_client *rtcp;
    uint16_t local_rtp_port;
    uint16_t local_rtcp_port;
    uint16_t peer_rtp_port;
    uint16_t peer_rtcp_port;
    struct {
        zms_rtsp_session *rs;
        int track_idx;
    } rtp_cb;
} zms_rtsp_session_udp_track;

struct zms_rtsp_session {
    unsigned session_no;
    zn_tcp_session *tcp;
    zms_rtsp_splitter *splitter;
    /** ── 推流侧（RECORD，GB/RTSP 推流）──────────────────────────────── */
    struct {
        zms_sdp_session publish_sdp;
        int publish_track_setup[ZMS_SDP_TRACK_MAX];
        zms_live_ingest *ingress;
        void *zm_rtp; /**< zms_zm_rtp_ingest（固定） */
        int logged_record;
        int logged_record_h264;
        unsigned record_rtp_count;
        zms_codec_id audio_codec;
        int audio_rate;
        int audio_channels;
        uint32_t video_clock_hz;
        uint32_t audio_clock_hz;
    } pb;

    /** ── 播放侧（PLAY）──────────────────────────────────────────────── */
    struct {
        zms_egress_source src;
        zms_gop_reader *gop_reader;
        zms_vod_buffer_reader *vod_reader;
        zms_vod_play_lane *vod_lane;
        zms_rtp_muxer *rtp_muxer;
        zms_egress_pipeline *egress_pipe;
        zms_rtp_play_sender *sender;
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
    char vhost[ZMS_VHOST_MAX];
    int low_latency;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    unsigned cseq;
    zms_rtsp_session_mode mode;
    /* RTP 通道号 */
    uint8_t video_rtp_ch;
    uint8_t video_rtcp_ch;
    uint8_t audio_rtp_ch;
    uint8_t audio_rtcp_ch;
    int close_pending;
    int destroy_scheduled;
    zms_media_source *source;
    char params[ZMS_AUTH_PARAMS_MAX];
    zms_codec_id audio_codec;
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
    zms_rtp_play_sender *play_sender;
    zn_poller *poller;
    zms_rtsp_rtp_mode rtp_mode;
    char peer_ip[64];
    /** SDP o= / c= 对外公告地址（来自 zms_rtsp_server_opts.advertise_host） */
    char advertise_host[64];
    zms_rtsp_session_udp_track udp_tracks[ZMS_SDP_TRACK_MAX];
    void *zm_rtp; /**< zms_zm_rtp_ingest（固定） */
    unsigned play_udp_video_send_ok;
    unsigned play_udp_video_send_fail;
    /** RTSP 信令响应缓冲（buf_pool，按需扩到 ZMS_MEDIA_IO_BUF_SIZE） */
    uint8_t *resp_buf;
    size_t resp_cap;
    uint8_t *play_interleaved_buf;
    size_t play_interleaved_cap;
};

typedef struct zms_rtsp_vod_play_ctx {
    zms_rtsp_session *rs;
    zms_media_source *src;
    uint64_t seek_ms;
    zms_vod_play_lane *lane;
    int is_replay;
    int was_paused;
    int did_seek;
    int range_parsed;
    int range_now;
    double scale;
    char range_req[160];
} zms_rtsp_vod_play_ctx;

typedef void (*zms_rtsp_method_fn)(zms_rtsp_session *rs, const zms_rtsp_message *msg);

void zms_rtsp_session_send_resp(zms_rtsp_session *rs, int code, const char *reason,
                                const char *extra, const char *body, size_t body_len);
void zms_rtsp_session_parse_url(const char *url, char *app, char *stream);
/** 记录请求 URL 的查询串（鉴权参数），供 RECORD/PLAY 验签使用。 */
void zms_rtsp_session_capture_params(zms_rtsp_session *rs, const char *url);
void zms_rtsp_url_vhost(const char *url, char *out, size_t cap);

void zms_rtsp_multicast_configure(const char *addr, unsigned port_base, int ttl);
int zms_rtsp_multicast_enabled(void);
const char *zms_rtsp_multicast_addr(void);
unsigned zms_rtsp_multicast_port_base(void);
int zms_rtsp_multicast_ttl(void);
int zms_rtsp_session_setup_multicast_play(zms_rtsp_session *rs, int track_idx, char *extra,
                                          size_t extra_cap);
int zms_rtsp_session_match(zms_rtsp_session *rs, const char *session_hdr);
int zms_rtsp_session_setup_track_id(const char *url, const zms_sdp_session *sdp);
int zms_rtsp_session_record_any_track_setup(const zms_rtsp_session *rs);
int zms_rtsp_session_record_track_by_channel(const zms_rtsp_session *rs, uint8_t channel);
void zms_rtsp_session_load_audio_params(zms_rtsp_session *rs);
/** VOD 总时长（ms）：publisher 元数据 + 文件 probe 回退 */
uint64_t zms_rtsp_session_vod_duration_ms(const zms_rtsp_session *rs);
/** VOD 当前播放位置（ms）：优先已发送视频 RTP 时间 */
uint64_t zms_rtsp_session_vod_play_position_ms(const zms_rtsp_session *rs);
void zms_rtsp_session_build_sdp(zms_rtsp_session *rs, char *out, size_t cap);
void zms_rtsp_session_send_rtp_interleaved(zms_rtsp_session *rs, uint8_t channel,
                                           const uint8_t *rtp, size_t rtp_len);
void zms_rtsp_session_send_rtcp_srs(zms_rtsp_session *rs);
void zms_rtsp_session_write_interleaved(void *user, uint8_t channel, const uint8_t *payload,
                                        size_t len);
void zms_rtsp_session_publisher_kick(void *ctx, int force);

void zms_rtsp_session_handle_options(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_announce(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_describe(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_setup(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_record(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_play(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_pause(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_get_parameter(zms_rtsp_session *rs, const zms_rtsp_message *msg);
void zms_rtsp_session_handle_teardown(zms_rtsp_session *rs, const zms_rtsp_message *msg);

void zms_rtsp_session_on_message(const zms_rtsp_message *msg, void *user);
void zms_rtsp_session_on_rtp(uint8_t channel, const uint8_t *data, size_t len, void *user);
void zms_rtsp_session_play_on_rtcp(zms_rtsp_session *rs, const uint8_t *data, size_t len);
void zms_rtsp_session_record_payload_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_record_input_rtp_raw(zms_rtsp_session *rs, int track_idx, const uint8_t *data,
                                           size_t len);
int zms_rtsp_session_setup_udp_record(zms_rtsp_session *rs, int track_idx,
                                      const char *transport_hdr, char *extra, size_t extra_cap);
int zms_rtsp_session_setup_udp_play(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                                    char *extra, size_t extra_cap);
void zms_rtsp_session_udp_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_send_media(zms_rtsp_session *rs, uint8_t channel, const uint8_t *payload,
                                 size_t len);
void zms_rtsp_session_play_tick(zms_rtsp_session *rs);
void zms_rtsp_session_play_kick(zms_rtsp_session *rs);
zn_err_t zms_rtsp_session_play_vod_lane_attach(zms_rtsp_session *rs, zms_media_source *src,
                                                uint64_t seek_ms);
void zms_rtsp_session_egress_close(zms_rtsp_session *rs);
void zms_rtsp_session_play_mux_destroy(zms_rtsp_session *rs);
void zms_rtsp_session_fill_play_ctx(zms_rtsp_session *rs, zms_rtp_play_run_ctx *ctx);
zms_rtsp_play_rtp_info_args zms_rtsp_session_rtp_info_args(zms_rtsp_session *rs, uint32_t anchor_ms,
                                                           int vod_linear);
void zms_rtsp_session_play_mux_create(zms_rtsp_session *rs);
void zms_rtsp_session_play_try_rtcp_boot(zms_rtsp_session *rs);
void zms_rtsp_session_send_rtcp_sr_tracks(zms_rtsp_session *rs, int send_video, int send_audio);
void zms_rtsp_session_vod_play_continue(zms_rtsp_session *rs, int is_replay, int was_paused,
                                        int did_seek, uint64_t seek_ms, double scale,
                                        const char *range_req, int range_parsed, int range_now);
int zms_rtsp_session_vod_open_try_async(zms_rtsp_vod_play_ctx *job);

void zms_rtsp_session_teardown(zms_rtsp_session *rs);
void zms_rtsp_session_schedule_destroy(zms_rtsp_session *rs, zn_tcp_session *session);

#endif /* ZMS_SRC_PROTOCOL_RTSP_SESSION_INTERNAL_H */
