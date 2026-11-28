/**
 * @file client_internal.h
 * @brief RTSP 客户端私有结构。
 */
#ifndef ZCS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H
#define ZCS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H

#include "zcs/protocol/rtsp/client.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zcs/protocol/rtsp/splitter.h"
#include "zcs/protocol/rtsp/parser.h"
#include "zcs/media/wire/rtp_receiver.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tls_client.h"
#include "zn/net/udp_client.h"
#include "zn/util/timer.h"
#include <stdint.h>

#define ZCS_RTSP_CLIENT_UDP_MAX ZCS_SDP_TRACK_MAX
/** scheme(5)+://+host(256)+:65535+path(512) 上界 */
#define ZCS_RTSP_URL_FULL_MAX 1024
/** 完整 URL + track control 拼接上界 */
#define ZCS_RTSP_SETUP_URL_MAX (ZCS_RTSP_URL_FULL_MAX + 128 + 8)

typedef enum zcs_rtsp_client_state {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_OPTIONS,
    ST_DESCRIBE,
    ST_SETUP,
    ST_PLAY,
    ST_PLAYING,
    ST_ERROR,
} zcs_rtsp_client_state;

typedef struct zcs_rtsp_url {
    char host[256];
    uint16_t port;
    char path[512];
    char full[ZCS_RTSP_URL_FULL_MAX];
    char user[64];
    char pass[128];
    const char *scheme;
    int use_tls;
} zcs_rtsp_url;

typedef struct zcs_rtsp_client_udp_track {
    struct zcs_rtsp_client *owner;
    int track_idx;
    zn_udp_client *rtp;
    zn_udp_client *rtcp;
    uint16_t local_rtp;
    uint16_t local_rtcp;
    uint16_t peer_rtp;
    uint16_t peer_rtcp;
    uint32_t rtp_ssrc;
    int active;
} zcs_rtsp_client_udp_track;

struct zcs_rtsp_client {
    zcs_rtsp_client_opts opts;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    zcs_rtsp_splitter *splitter;
    zcs_rtp_receiver *receiver;
    void *zm_rtp; /**< zcs_zm_rtp_ingest（固定） */
    zcs_sdp_session session;
    zcs_rtsp_url url;
    zcs_rtsp_client_state state;
    zcs_rtsp_rtp_mode requested_rtp_mode;
    zcs_rtsp_rtp_mode rtp_mode;
    int auto_udp_tried;
    int auth_retry_done;
    char digest_realm[128];
    char digest_nonce[128];
    unsigned cseq;
    char session_id[128];
    char content_base[512];
    unsigned setup_index;
    zcs_rtsp_client_udp_track udp[ZCS_RTSP_CLIENT_UDP_MAX];
    int stopping;
    int teardown_sent;
    zn_poller_timer *close_timer;
    int retry_count;
    int reconnect_delay_ms;
    int failed_count;
    zn_poller_timer *retry_timer;
};

void rtsp_client_emit_rtp_frame(zcs_rtsp_client *c, int track_index, const zcs_rtp_packet *pkt);
void rtsp_client_fail(zcs_rtsp_client *c, zn_err_t err);

void rtsp_client_udp_teardown(zcs_rtsp_client *c);
zn_err_t rtsp_client_udp_prepare_track(zcs_rtsp_client *c, unsigned track_idx);
void rtsp_client_udp_apply_setup_response(zcs_rtsp_client *c, unsigned track_idx,
                                          const char *transport);
void rtsp_client_udp_on_play(zcs_rtsp_client *c);
int rtsp_client_udp_build_setup_extra(zcs_rtsp_client *c, unsigned track_idx, char *buf,
                                      size_t cap);

#endif /* ZCS_SRC_PROTOCOL_RTSP_CLIENT_INTERNAL_H */
