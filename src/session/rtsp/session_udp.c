/**
 * @file session_udp.c
 * @brief RTSP UDP 传输：RTP/RTCP socket 绑定与收发。
 */
#include "session/rtsp/session_internal.h"
#include "zms/media/wire/rtp_packet.h"
#include "zn/net/socket.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_mcast_addr[64] = "239.255.42.99";
static unsigned g_mcast_base;
static int g_mcast_ttl = 16;

/* 全局组播配置：地址/起始端口/TTL。TTL<=0 回退 16（跨多级交换机但也避免无限
 * 泛洪到公网）。这些是进程级单例，启动时配置一次即可。 */
void zms_rtsp_multicast_configure(const char *addr, unsigned port_base, int ttl)
{
    if (addr && addr[0]) {
        strncpy(g_mcast_addr, addr, sizeof(g_mcast_addr) - 1);
        g_mcast_addr[sizeof(g_mcast_addr) - 1] = 0;
    }
    g_mcast_base = port_base;
    g_mcast_ttl = ttl > 0 ? ttl : 16;
}

int zms_rtsp_multicast_enabled(void)
{
    return g_mcast_base != 0 && g_mcast_addr[0] != 0;
}

const char *zms_rtsp_multicast_addr(void)
{
    return g_mcast_addr;
}

unsigned zms_rtsp_multicast_port_base(void)
{
    return g_mcast_base;
}

int zms_rtsp_multicast_ttl(void)
{
    return g_mcast_ttl;
}

static void zms_rtsp_session_refresh_peer_ip(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    rs->peer_ip[0] = '\0';
    if (!rs->tcp) {
        return;
    }
    zn_socket *sock = zn_tcp_session_socket(rs->tcp);
    if (sock) {
        (void)zn_socket_get_peer(sock, rs->peer_ip, sizeof(rs->peer_ip), NULL);
    }
}

/* 解析 Transport 里的 client_port=RTP-RTCP。必须是成对的两个非零端口（RTCP 端口
 * 通常 = RTP+1）；缺一个或任一为 0 都视为非法——我们发送时要分别定向到这两个端口，
 * 缺一不可。 */
static int parse_client_port(const char *transport, uint16_t *rtp_port, uint16_t *rtcp_port)
{
    const char *p = transport ? strstr(transport, "client_port=") : NULL;
    if (!p) {
        return -1;
    }
    p += strlen("client_port=");
    unsigned rtp = 0, rtcp = 0;
    if (sscanf(p, "%u-%u", &rtp, &rtcp) < 2) {
        return -1;
    }
    if (rtp == 0 || rtcp == 0) {
        return -1;
    }
    *rtp_port = (uint16_t)rtp;
    *rtcp_port = (uint16_t)rtcp;
    return 0;
}

static int peer_ip_match(const zms_rtsp_session *rs, const char *from_ip)
{
    if (!rs || !from_ip || !from_ip[0]) {
        return 0;
    }
    if (!rs->peer_ip[0]) {
        return 1;
    }
    return strcmp(rs->peer_ip, from_ip) == 0;
}

/** PLAY：SETUP 已知 client_port 时接受任意源 IP（WSL↔Windows NAT、TCP peer=127.0.0.1）*/
/* 注意放宽策略：端口对了还要 IP 兜底——NAT 场景下实际发包源端口可能被改写、
 * 与 SETUP 声明的 client_port 不同，只认端口会漏包。故“端口匹配 OR IP 匹配”
 * 任一成立即接受，两者都不满足才丢。 */
static int play_udp_packet_accept(const zms_rtsp_session *rs, const zms_rtsp_session_udp_track *ut,
                                  const char *from_ip, uint16_t from_port, int rtcp_sock)
{
    if (!from_ip || !from_ip[0] || !from_port) {
        return 0;
    }
    if (!rs || !ut) {
        return 0;
    }
    if (rtcp_sock) {
        if (from_port == ut->peer_rtcp_port) {
            return 1;
        }
    } else if (from_port == ut->peer_rtp_port) {
        return 1;
    }
    return peer_ip_match(rs, from_ip);
}

static void zms_rtsp_session_note_udp_peer(zms_rtsp_session *rs, const char *from_ip, int track_idx)
{
    if (!rs || !from_ip || !from_ip[0]) {
        return;
    }
    if (strcmp(rs->peer_ip, from_ip) == 0) {
        return;
    }
    if (track_idx >= 0) {
        zn_info("RTSP #%u UDP NAT: peer %s -> %s (track=%d)", rs->session_no,
                 rs->peer_ip[0] ? rs->peer_ip : "?", from_ip, track_idx);
    } else {
        zn_info("RTSP #%u UDP NAT: peer %s -> %s (all tracks)", rs->session_no,
                 rs->peer_ip[0] ? rs->peer_ip : "?", from_ip);
    }
    snprintf(rs->peer_ip, sizeof(rs->peer_ip), "%s", from_ip);
}

/*
 * 学习 RTP 打洞得到的对端地址并回填发送目标。
 *
 * 直播推流（RECORD）时客户端不一定会真的从 SETUP 声明的端口发 RTP；NAT 后
 * 实际源端口常被改写。只有收到首包才能确定“能回包的真实地址”，因此收到
 * 就更新 peer 元组。track_idx 用于日志区分是哪条轨触发的地址变更。
 */
static void udp_learn_rtp_peer(zms_rtsp_session *rs, zms_rtsp_session_udp_track *ut,
                               const char *from_ip, uint16_t from_port)
{
    if (!ut || !ut->rtp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtp_port != from_port) {
        ut->peer_rtp_port = from_port;
    }
    zn_udp_client_set_peer(ut->rtp, from_ip, from_port);
    if (rs) {
        zms_rtsp_session_note_udp_peer(rs, from_ip, ut->rtp_cb.track_idx);
    }
}

static void udp_learn_rtcp_peer(zms_rtsp_session *rs, zms_rtsp_session_udp_track *ut,
                                const char *from_ip, uint16_t from_port)
{
    if (!ut || !ut->rtcp || !from_ip || !from_port) {
        return;
    }
    if (ut->peer_rtcp_port != from_port) {
        ut->peer_rtcp_port = from_port;
    }
    zn_udp_client_set_peer(ut->rtcp, from_ip, from_port);
    if (rs) {
        zms_rtsp_session_note_udp_peer(rs, from_ip, ut->rtp_cb.track_idx);
    }
}

/** RECORD：与 PLAY 相同，SETUP 已知 client_port 时接受 NAT 源 IP */
static int record_udp_packet_accept(const zms_rtsp_session *rs,
                                    const zms_rtsp_session_udp_track *ut, const char *from_ip,
                                    uint16_t from_port, int rtcp_sock)
{
    if (!from_ip || !from_ip[0] || !from_port) {
        return 0;
    }
    if (!rs || !ut) {
        return 0;
    }
    if (rtcp_sock) {
        if (from_port == ut->peer_rtcp_port) {
            return 1;
        }
    } else if (from_port == ut->peer_rtp_port) {
        return 1;
    }
    return peer_ip_match(rs, from_ip);
}

/*
 * RECORD（推流）的 RTP 收包：先做源校验与 NAT 学习，再丢 RTCP，最后交给会话
 * 解析。注意 zms_rtp_is_rtcp 的判断在 learn 之后：RTCP 混在同一 socket 上到达
 * 时也要借它刷新对端地址，故先学后丢。
 */
static void record_udp_on_rtp_packet(zn_udp_client *client, const void *data, size_t len,
                                     const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    int track_idx = ut ? ut->rtp_cb.track_idx : -1;
    (void)client;

    if (!rs || track_idx < 0 || rs->mode != ZMS_RTSP_SESSION_MODE_RECORD || !data || len < 12) {
        return;
    }
    if (!record_udp_packet_accept(rs, ut, from_ip, from_port, 0)) {
        return;
    }

    udp_learn_rtp_peer(rs, ut, from_ip, from_port);
    zms_rtsp_session_note_udp_peer(rs, from_ip, track_idx);

    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }

    zms_rtsp_session_record_input_rtp_raw(rs, track_idx, (const uint8_t *)data, len);
}

static void record_udp_on_rtcp_packet(zn_udp_client *client, const void *data, size_t len,
                                      const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;
    if (!rs || !ut || !data || len < 8) {
        return;
    }
    if (!record_udp_packet_accept(rs, ut, from_ip, from_port, 1)) {
        return;
    }
    udp_learn_rtcp_peer(rs, ut, from_ip, from_port);
    (void)data;
    (void)len;
}

static void play_udp_sync_rtp_peer(zms_rtsp_session_udp_track *ut, const char *from_ip)
{
    if (!ut || !ut->rtp || !from_ip || !from_ip[0] || !ut->peer_rtp_port) {
        return;
    }
    zn_udp_client_set_peer(ut->rtp, from_ip, ut->peer_rtp_port);
}

/** 同一播放器各 track 共享 IP（NAT 学习一次即可修正全部发送目标）*/
static void play_udp_sync_all_tracks(zms_rtsp_session *rs, const char *from_ip)
{
    int i;

    if (!rs || !from_ip || !from_ip[0]) {
        return;
    }
    zms_rtsp_session_note_udp_peer(rs, from_ip, -1);
    for (i = 0; i < ZMS_SDP_TRACK_MAX; ++i) {
        zms_rtsp_session_udp_track *ut = &rs->udp_tracks[i];

        if (ut->rtp && ut->peer_rtp_port) {
            zn_udp_client_set_peer(ut->rtp, from_ip, ut->peer_rtp_port);
        }
        if (ut->rtcp && ut->peer_rtcp_port) {
            zn_udp_client_set_peer(ut->rtcp, from_ip, ut->peer_rtcp_port);
        }
    }
}

/* PLAY 收到 RTCP：学习/校正发送目标后，若会话确实在播放且是合法 RTCP，再把
 * 内容交给上层（用于 SR/RR 统计）。打洞与统计分离，避免偶发的 RTCP 混流打断
 * 状态机。 */
static void play_udp_on_rtcp_packet(zn_udp_client *client, const void *data, size_t len,
                                    const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;

    if (!rs || !ut || !data || len < 8) {
        return;
    }
    if (!play_udp_packet_accept(rs, ut, from_ip, from_port, 1)) {
        return;
    }
    udp_learn_rtcp_peer(rs, ut, from_ip, from_port);
    /* 客户端 RTCP 源用于校正 RTP 发送目标（NAT / WSL host / ffplay）。 */
    play_udp_sync_all_tracks(rs, from_ip);
    if (rs->mode == ZMS_RTSP_SESSION_MODE_PLAY && zms_rtp_is_rtcp(data, len)) {
        zms_rtsp_session_play_on_rtcp(rs, (const uint8_t *)data, len);
    }
}

static void play_udp_on_rtp_packet(zn_udp_client *client, const void *data, size_t len,
                                   const char *from_ip, uint16_t from_port, void *user)
{
    zms_rtsp_session_udp_track *ut = (zms_rtsp_session_udp_track *)user;
    zms_rtsp_session *rs = ut ? ut->rtp_cb.rs : NULL;
    (void)client;
    (void)data;
    (void)len;
    if (!rs || !ut) {
        return;
    }
    if (!play_udp_packet_accept(rs, ut, from_ip, from_port, 0)) {
        return;
    }
    /* PLAY 打洞：客户端 RTP 端口首包（含 RTCP 混流） */
    udp_learn_rtp_peer(rs, ut, from_ip, from_port);
    play_udp_sync_rtp_peer(ut, from_ip);
}

static void udp_teardown_track(zms_rtsp_session *rs, int track_idx)
{
    if (!rs || track_idx < 0 || (unsigned)track_idx >= ZMS_SDP_TRACK_MAX) {
        return;
    }
    zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track_idx];
    if (ut->rtp) {
        zn_udp_client_destroy(ut->rtp);
        ut->rtp = NULL;
    }
    if (ut->rtcp) {
        zn_udp_client_destroy(ut->rtcp);
        ut->rtcp = NULL;
    }
    if (ut->local_rtp_port) {
        zms_rtsp_transport_release_ports(ut->local_rtp_port);
        ut->local_rtp_port = 0;
        ut->local_rtcp_port = 0;
    }
    memset(ut, 0, sizeof(*ut));
}

void zms_rtsp_session_udp_teardown(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    for (int i = 0; i < ZMS_SDP_TRACK_MAX; ++i) {
        udp_teardown_track(rs, i);
    }
}

/*
 * 发送媒体到客户端。channel 的编码约定：每条轨占两个交织通道，
 * channel/2 = 轨道序号，channel%2 = 0(RTP)/1(RTCP)。这是 RTSP over TCP 的
 * 交织约定，UDP 模式复用同一套 channel 编号，故需反转回 track/类型。
 *
 * UDP 发不出去时按“视频失败计数”做节流告警：socket 未 SETUP 或 send 失败可能
 * 每帧都发生（几十 Hz），全量打日志会淹没输出，只在前几次提示。
 * 计数只对视频累加——音频丢包对观感影响小，不值得单独刷日志。
 */
void zms_rtsp_session_send_media(zms_rtsp_session *rs, uint8_t channel, const uint8_t *payload,
                                 size_t len)
{
    if (!rs || !payload || len == 0) {
        return;
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_UDP) {
        int track = (int)(channel / 2);
        int is_rtcp = channel % 2;
        if (track < 0 || track >= ZMS_SDP_TRACK_MAX) {
            return;
        }
        zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track];
        zn_udp_client *cli = is_rtcp ? ut->rtcp : ut->rtp;
        if (!cli) {
            if (!is_rtcp && track == 0 && rs->pl.udp_video_send_fail < 3) {
                zn_warn("RTSP PLAY UDP: drop track=%d ch=%u (socket not SETUP)", track,
                         (unsigned)channel);
            }
            if (!is_rtcp) {
                rs->pl.udp_video_send_fail++;
            }
            return;
        }
        if (zn_udp_client_send(cli, payload, len) != ZN_OK) {
            if (!is_rtcp && rs->pl.udp_video_send_fail < 5) {
                zn_warn("RTSP PLAY UDP: send failed track=%d ch=%u len=%u", track,
                         (unsigned)channel, (unsigned)len);
            }
            if (!is_rtcp) {
                rs->pl.udp_video_send_fail++;
            }
            return;
        }
        if (!is_rtcp && track == 0 && rs->pl.udp_video_send_ok++ == 0) {
            const char *dest = rs->peer_ip[0] ? rs->peer_ip : "?";
            zn_info("RTSP #%u PLAY UDP first video send: %u bytes -> %s:%u", rs->session_no,
                     (unsigned)len, dest, (unsigned)ut->peer_rtp_port);
        }
        return;
    }
    zms_rtsp_session_send_rtp_interleaved(rs, channel, payload, len);
}

static int setup_udp_track(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                           char *extra, size_t extra_cap, const zn_udp_client_ops_t *rtp_ops,
                           const zn_udp_client_ops_t *rtcp_ops, const char *log_tag)
{
    if (!rs || !transport_hdr || !extra || extra_cap < 128 || track_idx < 0 ||
        (unsigned)track_idx >= ZMS_SDP_TRACK_MAX || !rtp_ops || !rtcp_ops) {
        return -1;
    }

    zms_rtsp_session_refresh_peer_ip(rs);
    if (!rs->peer_ip[0]) {
        zn_warn("RTSP SETUP UDP: no peer ip");
        return -1;
    }

    uint16_t client_rtp = 0, client_rtcp = 0;
    if (parse_client_port(transport_hdr, &client_rtp, &client_rtcp) != 0) {
        zn_warn("RTSP SETUP UDP: missing client_port");
        return -1;
    }

    zms_rtsp_session_udp_track *ut = &rs->udp_tracks[track_idx];
    if (ut->rtp) {
        return -1;
    }

    uint16_t local_rtp = 0, local_rtcp = 0;
    if (zms_rtsp_transport_acquire_ports(&local_rtp, &local_rtcp) != ZN_OK) {
        zn_warn("RTSP SETUP UDP: port pool exhausted");
        return -1;
    }

    zn_udp_client_opts_t copts = {rs->poller, rtp_ops, ut};
    ut->rtp = zn_udp_client_create(&copts);
    copts.ops = rtcp_ops;
    zn_udp_client *rtcp_cli = zn_udp_client_create(&copts);
    if (!ut->rtp || !rtcp_cli) {
        if (ut->rtp) {
            zn_udp_client_destroy(ut->rtp);
            ut->rtp = NULL;
        }
        if (rtcp_cli) {
            zn_udp_client_destroy(rtcp_cli);
        }
        zms_rtsp_transport_release_ports(local_rtp);
        return -1;
    }
    ut->rtcp = rtcp_cli;
    ut->rtp_cb.rs = rs;
    ut->rtp_cb.track_idx = track_idx;
    ut->peer_rtp_port = client_rtp;
    ut->peer_rtcp_port = client_rtcp;
    ut->local_rtp_port = local_rtp;
    ut->local_rtcp_port = local_rtcp;

    /*
     * 服务端始终 bind 0.0.0.0：WSL NAT 下 TCP peer 常为 127.0.0.1，但 Windows
     * ffplay 的 RTCP 从宿主机地址打入；若绑 loopback 则收不到打洞包。
     */
    if (zn_udp_client_bind(ut->rtp, "0.0.0.0", local_rtp, 1) != ZN_OK ||
        zn_udp_client_bind(ut->rtcp, "0.0.0.0", local_rtcp, 1) != ZN_OK) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }

    zn_udp_client_set_peer(ut->rtp, rs->peer_ip, client_rtp);
    zn_udp_client_set_peer(ut->rtcp, rs->peer_ip, client_rtcp);

    uint8_t ich = (uint8_t)(track_idx * 2);
    if (track_idx == 0) {
        rs->video_rtp_ch = ich;
        rs->video_rtcp_ch = (uint8_t)(ich + 1);
    } else if (track_idx == 1) {
        rs->audio_rtp_ch = ich;
        rs->audio_rtcp_ch = (uint8_t)(ich + 1);
    }

    uint32_t ssrc = (track_idx == 0) ? rs->pl.video_rtp_ssrc : rs->pl.audio_rtp_ssrc;
    if (!ssrc) {
        ssrc = (track_idx == 0) ? 0x12345678u : 0x87654321u;
    }

    int n = snprintf(extra, extra_cap,
                     "Transport: RTP/AVP/UDP;unicast;"
                     "client_port=%u-%u;"
                     "server_port=%u-%u;"
                     "ssrc=%08X\r\n"
                     "Session: %s;timeout=60\r\n",
                     (unsigned)client_rtp, (unsigned)client_rtcp, (unsigned)local_rtp,
                     (unsigned)local_rtcp, ssrc, rs->session_id);
    if (n < 0 || (size_t)n >= extra_cap) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }

    zn_info("RTSP SETUP 200 UDP %s: track=%d client=%u-%u server=%u-%u peer=%s ch=%u-%u", log_tag,
             track_idx, (unsigned)client_rtp, (unsigned)client_rtcp, (unsigned)local_rtp,
             (unsigned)local_rtcp, rs->peer_ip, (unsigned)ich, (unsigned)(ich + 1));
    return 0;
}

int zms_rtsp_session_setup_udp_record(zms_rtsp_session *rs, int track_idx,
                                      const char *transport_hdr, char *extra, size_t extra_cap)
{
    static const zn_udp_client_ops_t rtp_ops = {record_udp_on_rtp_packet};
    static const zn_udp_client_ops_t rtcp_ops = {record_udp_on_rtcp_packet};
    return setup_udp_track(rs, track_idx, transport_hdr, extra, extra_cap, &rtp_ops, &rtcp_ops,
                           "RECORD");
}

int zms_rtsp_session_setup_udp_play(zms_rtsp_session *rs, int track_idx, const char *transport_hdr,
                                    char *extra, size_t extra_cap)
{
    static const zn_udp_client_ops_t rtp_ops = {play_udp_on_rtp_packet};
    static const zn_udp_client_ops_t rtcp_ops = {play_udp_on_rtcp_packet};
    return setup_udp_track(rs, track_idx, transport_hdr, extra, extra_cap, &rtp_ops, &rtcp_ops,
                           "PLAY");
}

int zms_rtsp_session_setup_multicast_play(zms_rtsp_session *rs, int track_idx, char *extra,
                                          size_t extra_cap)
{
    static const zn_udp_client_ops_t rtp_ops = {play_udp_on_rtp_packet};
    static const zn_udp_client_ops_t rtcp_ops = {play_udp_on_rtcp_packet};
    zms_rtsp_session_udp_track *ut;
    zn_udp_client_opts_t copts;
    zn_udp_client *rtcp_cli;
    const char *group = zms_rtsp_multicast_addr();
    unsigned base = zms_rtsp_multicast_port_base();
    unsigned offset;
    unsigned rtp_port;
    uint8_t ich;
    uint32_t ssrc;
    int n;

    if (!rs || !extra || extra_cap < 128 || track_idx < 0 ||
        (unsigned)track_idx >= ZMS_SDP_TRACK_MAX || !zms_rtsp_multicast_enabled()) {
        return -1;
    }
    /* 组播端口按会话号散列到 base..base+2000 的窗口内，每轨占 2 个端口
     * (RTP/RTCP)。用固定步长而非随机，保证同一会话重连后落在同一组播地址，
     * 客户端缓存的地址仍然有效；2000 的窗口避免端口过早回绕到 65535。 */
    offset = (unsigned)((rs->session_no * 4u + (unsigned)track_idx * 2u) % 2000u);
    rtp_port = base + offset;
    if (rtp_port < 1024 || rtp_port > 65534u) {
        return -1;
    }
    ut = &rs->udp_tracks[track_idx];
    if (ut->rtp) {
        return -1;
    }

    copts.poller = rs->poller;
    copts.ops = &rtp_ops;
    copts.user = ut;
    ut->rtp = zn_udp_client_create(&copts);
    copts.ops = &rtcp_ops;
    rtcp_cli = zn_udp_client_create(&copts);
    if (!ut->rtp || !rtcp_cli) {
        if (ut->rtp) {
            zn_udp_client_destroy(ut->rtp);
            ut->rtp = NULL;
        }
        if (rtcp_cli) {
            zn_udp_client_destroy(rtcp_cli);
        }
        return -1;
    }
    ut->rtcp = rtcp_cli;
    ut->rtp_cb.rs = rs;
    ut->rtp_cb.track_idx = track_idx;
    ut->peer_rtp_port = (uint16_t)rtp_port;
    ut->peer_rtcp_port = (uint16_t)(rtp_port + 1);
    ut->local_rtp_port = 0; /* 组播发送不占端口池 */
    ut->local_rtcp_port = 0;

    (void)zn_udp_client_set_multicast(ut->rtp, zms_rtsp_multicast_ttl(), NULL);
    (void)zn_udp_client_set_multicast(ut->rtcp, zms_rtsp_multicast_ttl(), NULL);
    if (zn_udp_client_bind(ut->rtp, "0.0.0.0", 0, 0) != ZN_OK ||
        zn_udp_client_bind(ut->rtcp, "0.0.0.0", 0, 0) != ZN_OK) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }
    zn_udp_client_set_peer(ut->rtp, group, (uint16_t)rtp_port);
    zn_udp_client_set_peer(ut->rtcp, group, (uint16_t)(rtp_port + 1));
    rs->rtp_mode = ZMS_RTSP_RTP_UDP;

    ich = (uint8_t)(track_idx * 2);
    if (track_idx == 0) {
        rs->video_rtp_ch = ich;
        rs->video_rtcp_ch = (uint8_t)(ich + 1);
    } else if (track_idx == 1) {
        rs->audio_rtp_ch = ich;
        rs->audio_rtcp_ch = (uint8_t)(ich + 1);
    }
    ssrc = (track_idx == 0) ? rs->pl.video_rtp_ssrc : rs->pl.audio_rtp_ssrc;
    if (!ssrc) {
        ssrc = (track_idx == 0) ? 0x12345678u : 0x87654321u;
    }
    n = snprintf(extra, extra_cap,
                 "Transport: RTP/AVP;multicast;destination=%s;port=%u-%u;ttl=%d;ssrc=%08X\r\n"
                 "Session: %s;timeout=60\r\n",
                 group, rtp_port, rtp_port + 1, zms_rtsp_multicast_ttl(), ssrc, rs->session_id);
    if (n < 0 || (size_t)n >= extra_cap) {
        udp_teardown_track(rs, track_idx);
        return -1;
    }
    zn_info("RTSP SETUP 200 MULTICAST: track=%d group=%s port=%u-%u", track_idx, group, rtp_port,
             rtp_port + 1);
    return 0;
}
