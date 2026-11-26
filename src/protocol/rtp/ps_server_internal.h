/**
 * @file ps_server_internal.h
 * @brief RTP-PS 服务端私有结构。
 */
#ifndef ZMS_SRC_PROTOCOL_RTP_PS_SERVER_INTERNAL_H
#define ZMS_SRC_PROTOCOL_RTP_PS_SERVER_INTERNAL_H

#include "zms/core/auth.h"
#include "zms/protocol/rtp/ps_server.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/media/container/mpegps/mpegps_demuxer.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tcp_server.h"
#include "zn/net/udp_server.h"
#include <stdint.h>

struct zn_poller_pool;
typedef struct zn_poller_pool zn_poller_pool;

struct zms_rtp_ps_server_slot {
    char key[256];
    char vhost[ZMS_VHOST_MAX];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    uint16_t port;
    int payload_type;
    uint32_t ssrc_filter;
    int enable_ssrc_filter;
    int tcp_mode;
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    zn_udp_server *udp;
    zn_tcp_server *tcp_srv;
    zn_tcp_session *tcp_sess;
    zn_tcp_client *tcp_cli;
    char tcp_peer_ip[64];
    uint16_t tcp_peer_port;
    uint8_t *tcp_asm;
    size_t tcp_asm_len;
    size_t tcp_asm_cap;
    zms_live_ingest *ingress;
    zms_media_source *source;
    void *rtp_ps_decoder;
    zms_mpegps_demuxer *ps_demuxer;
    uint8_t *rtp_ps_assembly;
    size_t rtp_ps_assembly_cap;
    int publish_started;
    /** 信令层已授权（GB28181 协商）：开槽时跳过 token 验签，录制默认开启。 */
    int signaling_authorized;
    /** 外部直推开槽验签时得到的录制开关。 */
    int auth_enable_mp4;
    unsigned recv_pkts;
    unsigned raw_udp_pkts;
};

zms_rtp_ps_server_slot *zms_rtp_ps_server_slot_from_udp(zn_udp_server *udp);

#endif /* ZMS_SRC_PROTOCOL_RTP_PS_SERVER_INTERNAL_H */
