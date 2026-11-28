/**
 * @file transport.c
 * @brief RTSP Transport 头解析（RTP/AVP、interleaved、client/server port）。
 */
#include "zcs/protocol/rtsp/transport.h"
#include "protocol/rtsp/rtp_port.h"
#include "zb/os/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZCS_RTSP_UDP_SLOT_MAX 512

typedef struct {
    uint16_t port;
    int track_idx;
    int is_rtcp;
    zcs_rtsp_udp_on_packet_fn cb;
    void *user;
} rtsp_udp_slot;

static zb_mutex *g_udp_mtx;
static rtsp_udp_slot g_udp_slots[ZCS_RTSP_UDP_SLOT_MAX];
static zcs_rtp_port_pool *g_rtp_port_pool;

zcs_rtsp_rtp_mode zcs_rtsp_transport_parse_mode(const char *transport_hdr)
{
    if (!transport_hdr || !transport_hdr[0]) {
        return ZCS_RTSP_RTP_TCP;
    }
    if (strstr(transport_hdr, "interleaved") || strstr(transport_hdr, "TCP") ||
        strstr(transport_hdr, "tcp")) {
        return ZCS_RTSP_RTP_TCP;
    }
    if (strstr(transport_hdr, "client_port=") || strstr(transport_hdr, "server_port=")) {
        return ZCS_RTSP_RTP_UDP;
    }
    return ZCS_RTSP_RTP_TCP;
}

zcs_rtsp_rtp_mode zcs_rtsp_transport_resolve_mode(zcs_rtsp_rtp_mode mode)
{
    if (mode == ZCS_RTSP_RTP_UDP) {
        return ZCS_RTSP_RTP_UDP;
    }
    return ZCS_RTSP_RTP_TCP;
}

int zcs_rtsp_transport_parse_interleaved(const char *transport, uint8_t *rtp_ch, uint8_t *rtcp_ch)
{
    const char *p = transport ? strstr(transport, "interleaved=") : NULL;
    if (!p) {
        return -1;
    }
    unsigned rtp = 0, rtcp = 0;
    if (sscanf(p + 12, "%u-%u", &rtp, &rtcp) != 2) {
        return -1;
    }
    if (rtp_ch) {
        *rtp_ch = (uint8_t)rtp;
    }
    if (rtcp_ch) {
        *rtcp_ch = (uint8_t)rtcp;
    }
    return 0;
}

int zcs_rtsp_transport_parse_server_port(const char *transport, uint16_t *rtp_port,
                                         uint16_t *rtcp_port)
{
    const char *p = transport ? strstr(transport, "server_port=") : NULL;
    if (!p) {
        return -1;
    }
    p += strlen("server_port=");
    unsigned rtp = 0, rtcp = 0;
    if (sscanf(p, "%u-%u", &rtp, &rtcp) < 2 || rtp == 0 || rtcp == 0) {
        return -1;
    }
    if (rtp_port) {
        *rtp_port = (uint16_t)rtp;
    }
    if (rtcp_port) {
        *rtcp_port = (uint16_t)rtcp;
    }
    return 0;
}

int zcs_rtsp_transport_parse_client_port(const char *transport, uint16_t *rtp_port,
                                         uint16_t *rtcp_port)
{
    const char *p = transport ? strstr(transport, "client_port=") : NULL;
    if (!p) {
        return -1;
    }
    p += strlen("client_port=");
    unsigned rtp = 0, rtcp = 0;
    if (sscanf(p, "%u-%u", &rtp, &rtcp) < 2 || rtp == 0 || rtcp == 0) {
        return -1;
    }
    if (rtp_port) {
        *rtp_port = (uint16_t)rtp;
    }
    if (rtcp_port) {
        *rtcp_port = (uint16_t)rtcp;
    }
    return 0;
}

int zcs_rtsp_transport_parse_ssrc(const char *transport, uint32_t *ssrc)
{
    const char *p = transport ? strstr(transport, "ssrc=") : NULL;
    if (!p || !ssrc) {
        return -1;
    }
    unsigned v = 0;
    if (sscanf(p + 5, "%8x", &v) != 1) {
        return -1;
    }
    *ssrc = (uint32_t)v;
    return 0;
}

void zcs_rtsp_udp_registry_init(void)
{
    if (!g_udp_mtx) {
        g_udp_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
        memset(g_udp_slots, 0, sizeof(g_udp_slots));
    }
    if (!g_rtp_port_pool) {
        g_rtp_port_pool = zcs_rtp_port_pool_create(NULL);
    }
}

void zcs_rtsp_udp_registry_fini(void)
{
    if (g_udp_mtx) {
        zb_mutex_lock(g_udp_mtx);
    }
    memset(g_udp_slots, 0, sizeof(g_udp_slots));
    if (g_rtp_port_pool) {
        zcs_rtp_port_pool_destroy(g_rtp_port_pool);
        g_rtp_port_pool = NULL;
    }
    if (g_udp_mtx) {
        zb_mutex_unlock(g_udp_mtx);
        zb_mutex_destroy(g_udp_mtx);
        g_udp_mtx = NULL;
    }
}

zn_err_t zcs_rtsp_transport_acquire_ports(uint16_t *rtp_port, uint16_t *rtcp_port)
{
    zcs_rtsp_udp_registry_init();
    if (!g_rtp_port_pool) {
        return ZN_ERR_NOMEM;
    }
    return zcs_rtp_port_pool_acquire(g_rtp_port_pool, rtp_port, rtcp_port);
}

void zcs_rtsp_transport_release_ports(uint16_t rtp_port)
{
    if (g_rtp_port_pool) {
        zcs_rtp_port_pool_release(g_rtp_port_pool, rtp_port);
    }
}

zn_err_t zcs_rtsp_udp_registry_bind(uint16_t local_port, int track_idx, int is_rtcp,
                                     zcs_rtsp_udp_on_packet_fn cb, void *user)
{
    if (!local_port || !cb) {
        return ZN_ERR_INVALID;
    }
    zcs_rtsp_udp_registry_init();
    if (!g_udp_mtx) {
        return ZN_ERR_NOMEM;
    }

    zb_mutex_lock(g_udp_mtx);
    zn_err_t err = ZN_ERR_NOMEM;
    for (size_t i = 0; i < ZCS_RTSP_UDP_SLOT_MAX; ++i) {
        if (g_udp_slots[i].port == 0) {
            g_udp_slots[i].port = local_port;
            g_udp_slots[i].track_idx = track_idx;
            g_udp_slots[i].is_rtcp = is_rtcp ? 1 : 0;
            g_udp_slots[i].cb = cb;
            g_udp_slots[i].user = user;
            err = ZN_OK;
            break;
        }
    }
    zb_mutex_unlock(g_udp_mtx);
    return err;
}

void zcs_rtsp_udp_registry_unbind(uint16_t local_port)
{
    if (!local_port || !g_udp_mtx) {
        return;
    }
    zb_mutex_lock(g_udp_mtx);
    for (size_t i = 0; i < ZCS_RTSP_UDP_SLOT_MAX; ++i) {
        if (g_udp_slots[i].port == local_port) {
            memset(&g_udp_slots[i], 0, sizeof(g_udp_slots[i]));
            break;
        }
    }
    zb_mutex_unlock(g_udp_mtx);
}

void zcs_rtsp_udp_registry_dispatch(uint16_t local_port, const uint8_t *data, size_t len,
                                    const char *peer_ip, uint16_t peer_port)
{
    if (!local_port || !data || !len || !g_udp_mtx) {
        return;
    }

    zcs_rtsp_udp_on_packet_fn cb = NULL;
    void *user = NULL;
    int track_idx = 0;
    int is_rtcp = 0;

    zb_mutex_lock(g_udp_mtx);
    for (size_t i = 0; i < ZCS_RTSP_UDP_SLOT_MAX; ++i) {
        if (g_udp_slots[i].port == local_port) {
            cb = g_udp_slots[i].cb;
            user = g_udp_slots[i].user;
            track_idx = g_udp_slots[i].track_idx;
            is_rtcp = g_udp_slots[i].is_rtcp;
            break;
        }
    }
    zb_mutex_unlock(g_udp_mtx);

    if (cb) {
        cb(user, track_idx, is_rtcp, data, len, peer_ip, peer_port);
    }
}
