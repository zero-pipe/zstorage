/**
 * @file client_internal.h
 * @brief RTSP 客户端私有结构。
 */
#ifndef ZMS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H
#define ZMS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H

#include "zms/protocol/rtsp/client.h"
#include "zms/protocol/rtsp/transport.h"
#include "zms/protocol/rtsp/splitter.h"
#include "zms/protocol/rtsp/parser.h"
#include "zms/media/wire/rtp_receiver.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tls_client.h"
#include "zn/net/udp_client.h"
#include "zn/util/timer.h"
#include <stdint.h>

#define ZMS_RTSP_CLIENT_UDP_MAX ZMS_SDP_TRACK_MAX
/** scheme(5)+://+host(256)+:65535+path(512) 上界 */
#define ZMS_RTSP_URL_FULL_MAX 1024
/** 完整 URL + track control 拼接上界 */
#define ZMS_RTSP_SETUP_URL_MAX (ZMS_RTSP_URL_FULL_MAX + 128 + 8)

typedef enum zms_rtsp_client_state {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_OPTIONS,
    ST_DESCRIBE,
    ST_SETUP,
    ST_PLAY,
    ST_PLAYING,
    ST_ERROR,
} zms_rtsp_client_state;

typedef struct zms_rtsp_url {
    char host[256];
    uint16_t port;
    char path[512];
    char full[ZMS_RTSP_URL_FULL_MAX];
    char user[64];
    char pass[128];
    const char *scheme;
    int use_tls;
} zms_rtsp_url;

typedef struct zms_rtsp_client_udp_track {
    struct zms_rtsp_client *owner;
    int track_idx;
    zn_udp_client *rtp;
    zn_udp_client *rtcp;
    uint16_t local_rtp;
    uint16_t local_rtcp;
    uint16_t peer_rtp;
    uint16_t peer_rtcp;
    uint32_t rtp_ssrc;
    int active;
} zms_rtsp_client_udp_track;

struct zms_rtsp_client {
    zms_rtsp_client_opts opts;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    zms_rtsp_splitter *splitter;
    zms_rtp_receiver *receiver;
    void *zm_rtp; /**< zms_zm_rtp_ingest（固定） */
    zms_sdp_session session;
    zms_rtsp_url url;
    zms_rtsp_client_state state;
    zms_rtsp_rtp_mode requested_rtp_mode;
    zms_rtsp_rtp_mode rtp_mode;
    int auto_udp_tried;
    int auth_retry_done;
    char digest_realm[128];
    char digest_nonce[128];
    unsigned cseq;
    char session_id[128];
    char content_base[512];
    unsigned setup_index;
    zms_rtsp_client_udp_track udp[ZMS_RTSP_CLIENT_UDP_MAX];
    int stopping;
    int teardown_sent;
    zn_poller_timer *close_timer;
    int retry_count;
    int reconnect_delay_ms;
    int failed_count;
    zn_poller_timer *retry_timer;
};

void rtsp_client_emit_rtp_frame(zms_rtsp_client *c, int track_index, const zms_rtp_packet *pkt);
void rtsp_client_fail(zms_rtsp_client *c, zn_err_t err);

void rtsp_client_udp_teardown(zms_rtsp_client *c);
zn_err_t rtsp_client_udp_prepare_track(zms_rtsp_client *c, unsigned track_idx);
void rtsp_client_udp_apply_setup_response(zms_rtsp_client *c, unsigned track_idx,
                                          const char *transport);
void rtsp_client_udp_on_play(zms_rtsp_client *c);
int rtsp_client_udp_build_setup_extra(zms_rtsp_client *c, unsigned track_idx, char *buf,
                                      size_t cap);

#endif /* ZMS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H */
