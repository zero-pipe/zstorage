/**
 * @file session_internal.h
 * @brief WebRTC 会话私有结构。
 */
#ifndef ZMS_SRC_SESSION_WEBRTC_SESSION_INTERNAL_H
#define ZMS_SRC_SESSION_WEBRTC_SESSION_INTERNAL_H

#include "zms/media/codec/codec_id.h"
#include "zms/media/media_limits.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/source/egress_source.h"
#include "protocol/webrtc/ice_host.h"
#include "zn/net/udp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/buf.h"
#include "zn/util/timer.h"
#include <stddef.h>
#include <stdint.h>

#define ZMS_WEBRTC_SESSION_ID_LEN 32

typedef struct zms_webrtc_play_rtp_slot {
    uint8_t track;
    zn_buf *buf;
    uint64_t enq_ms;
} zms_webrtc_play_rtp_slot;

typedef struct zms_webrtc_nack_slot {
    uint16_t seq;
    zn_buf *buf;
} zms_webrtc_nack_slot;

struct zms_egress_pipeline;
struct zms_rtp_muxer;
struct zms_webrtc_dtls;
struct zms_webrtc_srtp;
struct zms_webrtc_ice;
struct zms_live_ingest;

typedef enum zms_webrtc_session_mode {
    ZMS_WEBRTC_SESSION_PLAY = 0,
    ZMS_WEBRTC_SESSION_PUBLISH = 1,
} zms_webrtc_session_mode;

typedef struct zms_webrtc_session zms_webrtc_session;

struct zms_webrtc_session {
    char id[ZMS_WEBRTC_SESSION_ID_LEN];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_webrtc_session_mode mode;
    zms_media_source *source;
    zms_egress_source play;
    struct zms_live_ingest *ingest;
    zn_udp_server *udp;
    zn_poller *poller;
    uint16_t port;
    /** ICE 运行时状态（协议 ICE 模块只依赖该子结构，见 ice_host.h）。 */
    zms_webrtc_ice_host ice_host;
    /** 信令侧 ICE 凭据（写进 offer/answer 并传给 ICE setup）。 */
    char local_ufrag[ZMS_WEBRTC_ICE_UFRAG_LEN + 1];
    char local_pwd[ZMS_WEBRTC_ICE_PWD_LEN + 1];
    char remote_ufrag[ZMS_WEBRTC_ICE_UFRAG_LEN + 1];
    char remote_pwd[ZMS_WEBRTC_ICE_PWD_LEN + 1];
    int dtls_ready;
    int dtls_as_client;
    int media_started;
    unsigned session_no;
    uint8_t video_pt;
    uint8_t audio_pt;
    zms_codec_id video_codec;
    zms_codec_id audio_codec;
    int audio_rate;
    int audio_channels;
    char video_mid[8];
    char audio_mid[8];
    int offer_audio_before_video;
    int offer_has_video;
    int offer_has_audio;
    int offer_has_data;
    int offer_media_count;
    char offer_media_type[4][16];
    char offer_media_mid[4][8];
    char data_mid[8];
    int answer_has_video;
    int answer_has_audio;
    struct zms_webrtc_dtls *dtls;
    struct zms_webrtc_srtp *video_srtp;
    struct zms_webrtc_srtp *audio_srtp;
    /** 接收方向（浏览器->设备）：客户端写密钥，用于 RTCP 反馈与对讲音频。 */
    struct zms_webrtc_srtp *video_srtp_recv;
    struct zms_webrtc_srtp *audio_srtp_recv;
    int talk_enabled;
    uint8_t talk_pt;
    int talk_codec;
    int talk_rate;
    uint16_t talk_seq;
    uint32_t talk_ts;
    uint64_t talk_rx_pkts;
    uint64_t talk_rx_bytes;
    struct zms_egress_pipeline *egress_pipe;
    struct zms_rtp_muxer *play_rtp_muxer;
    zn_poller_timer *pump_timer;
    int live_catchup_done;
    uint64_t play_start_ms;
    uint8_t *io_buf;
    size_t io_cap;
    /** ── RTP 发送/反馈传输状态（队列、NACK、TWCC、RTCP、节流与自适应）── */
    struct {
        zms_webrtc_play_rtp_slot *q;
        unsigned q_r;
        unsigned q_w;
        unsigned q_n;
        uint64_t q_bytes;
        uint64_t q_bytes_max;
        uint64_t q_dropped_bytes;
        uint64_t q_dropped_pkts;
        uint64_t q_alloc_fail;
        zms_webrtc_nack_slot *nack;
        unsigned nack_r;
        unsigned nack_w;
        unsigned nack_n;
        uint64_t nack_bytes;
        uint64_t nack_hits;
        uint64_t nack_misses;
        uint64_t nack_retransmits;
        unsigned rtcp_tick;
        int rtcp_video_sr_sent;
        int rtcp_audio_sr_sent;
        uint64_t rtcp_rr_pkts;
        uint32_t rtcp_fraction_lost;
        uint32_t rtcp_jitter;
        int64_t rtcp_rtt_ms;
        uint16_t twcc_seq;
        int twcc_ext_id;
        uint64_t twcc_fb_pkts;
        uint64_t twcc_received;
        uint64_t twcc_lost;
        uint64_t twcc_recovered;
        uint32_t twcc_loss_permille;
        uint32_t twcc_loss_ewma;
        uint32_t send_bps;
        uint64_t pace_tokens;
        uint64_t pace_last_ms;
        uint64_t pace_waits;
        int adapt_in_frame;
        int adapt_drop_frame;
        int adapt_wait_key;
        int adapt_dropping;
        int adapt_need_key;
        uint32_t adapt_window_bps;
        uint64_t adapt_window_ms;
        uint64_t adapt_window_bytes;
        uint64_t adapt_dropped_frames;
        uint64_t adapt_dropped_bytes;
        uint64_t adapt_wait_ewma_ms;
        unsigned adapt_hold_frames;
        uint64_t adapt_last_dec_ms;
        uint64_t adapt_last_inc_ms;
        uint64_t adapt_last_reset_ms;
        uint64_t adapt_resets;
        uint64_t pli_count;
        uint64_t pli_suppressed;
        uint64_t pli_last_ms;
        /* WHIP 入站速率窗口 */
        uint64_t whip_rx_pkts;
        uint64_t whip_rx_bytes;
        uint64_t whip_rx_dropped;
        uint64_t whip_win_start_ms;
        uint64_t whip_win_bytes;
        uint64_t whip_srtp_fail;
    } rt;

    uint64_t created_ms;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    unsigned play_trace_seq;
    unsigned play_out_logged;
    int play_pump_armed;
    int play_reader_attached;
    void *whip_ingress;
    uint8_t whip_h264_sps[256];
    uint8_t whip_h264_pps[256];
    size_t whip_h264_sps_len;
    size_t whip_h264_pps_len;
    int whip_h264_have_sprop;
    uint8_t play_h264_sps[256];
    uint8_t play_h264_pps[128];
    size_t play_h264_sps_len;
    size_t play_h264_pps_len;
    uint16_t play_h264_seq_bias;
};

zms_webrtc_session *zms_webrtc_session_create(zms_media_source *src, const char *app,
                                              const char *stream, zn_poller *poller);
zms_webrtc_session *zms_webrtc_session_create_publish(const char *vhost, const char *app,
                                                       const char *stream,
                                                      zn_poller *poller);
void zms_webrtc_session_destroy(zms_webrtc_session *s);
void zms_webrtc_session_teardown(zms_webrtc_session *s);
void zms_webrtc_session_destroy_all(void);
/** 媒体直接发送（TURN relay 时走 ICE；否则 peer 已知即 sendto）。 */
int zms_webrtc_session_send_udp(zms_webrtc_session *s, const void *data, size_t len);
/** 主动发起 DTLS ClientHello（发布端 / client 模式）。 */
void zms_webrtc_session_try_dtls_client(zms_webrtc_session *s);
/** 回收握手超时 / 空闲超时的会话。 */
void zms_webrtc_session_reap_idle(uint64_t now_ms);

zms_webrtc_session *zms_webrtc_session_find(const char *id);
/** 标记为主动连接会话：full ICE controlling + DTLS client，并重建 ICE agent。 */
int zms_webrtc_session_enable_client(zms_webrtc_session *s);
/** ICE restart：保留 DTLS/SRTP/媒体，仅重建 ICE agent。 */
int zms_webrtc_session_restart_ice(zms_webrtc_session *s);
/** 读取 offer 的 ICE ufrag 与首个 H264 PT（用于判断是否 ICE restart）。 */
int zms_webrtc_session_probe_offer(const char *offer, size_t offer_len, char *ufrag,
                                   size_t ufrag_cap, int *video_pt);

int zms_webrtc_session_io_buf_ensure(zms_webrtc_session *s);
void zms_webrtc_session_io_buf_release(zms_webrtc_session *s);

int zms_webrtc_session_build_answer(const zms_webrtc_session *s, const char *offer,
                                    size_t offer_len, char *answer, size_t answer_cap,
                                    size_t *answer_len);
int zms_webrtc_session_build_publish_answer(const zms_webrtc_session *s, const char *offer,
                                            size_t offer_len, char *answer, size_t answer_cap,
                                            size_t *answer_len);
/** 主动连接（WHEP/WHIP client）：生成 offer 与应用 answer。 */
int zms_webrtc_session_build_client_offer(zms_webrtc_session *s, int recvonly, char *offer,
                                          size_t offer_cap, size_t *offer_len);
int zms_webrtc_session_apply_client_answer(zms_webrtc_session *s, const char *answer,
                                           size_t answer_len);

void zms_webrtc_publish_apply_offer_h264_sprop(zms_webrtc_session *s);

/** 追加 trickle ICE 远端候选；成功返回解析到的候选数。 */
int zms_webrtc_session_add_remote_candidates(zms_webrtc_session *s, const char *sdp, size_t len);

void zms_webrtc_session_on_udp(zms_webrtc_session *s, const char *peer_ip, uint16_t peer_port,
                               const void *data, size_t len);

#endif /* ZMS_SRC_SESSION_WEBRTC_SESSION_INTERNAL_H */
