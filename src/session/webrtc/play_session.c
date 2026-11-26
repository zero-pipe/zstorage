/**
 * @file play_session.c
 * @brief WebRTC 播放会话：STUN/DTLS 与 RTP 输入。
 *
 * 播放会话从“浏览器已发来 STUN/DTLS”开始，到 DTLS 握手完成才真正出流：
 *   STUN binding → DTLS 握手 → 导出 SRTP 密钥 → 建 RTP muxer/egress 管线
 *   → 起 pump 定时器，每 tick 从 gop_queue 读帧、SRTP 加密、UDP 发送。
 * 入站除 DTLS 外还要处理浏览器回传的 RTCP（PLI/NACK/TWCC/RR）做重传、关键帧
 * 请求与自适应码率，以及可选的 SRTP 对讲音频。
 *
 * 并发：全部在会话所属 poller 线程上执行（STUN/DTLS/RTP 回调、pump 定时器都
 * 投递到同一 poller）；对讲 RTP 可能来自共享 UDP socket，但同样被派发到该
 * poller。内部字段无锁。Windows 下 pump 用 __try/__except 兜底崩溃，避免单
 * 会话异常拖垮进程。
 */
#include "session/webrtc/play_session.h"
#include "session/webrtc/play_rtp_internal.h"
#include "protocol/webrtc/media_gateway.h"
#include "protocol/webrtc/media_internal.h"
#include "session/webrtc/session_internal.h"
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/media/codec/codec_id.h"
#include "zms/egress/pipeline/egress_pipeline.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/core/gop/queue.h"
#include "zms/media/media_limits.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/pacing/egress_clock.h"
#include "zms/media/wire/rtcp.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/media/wire/srtp.h"
#include "zms/egress/pacing/egress_live_policy.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/egress/rtp/rtp_play_pump.h"
#include "protocol/webrtc/ice_internal.h"
#include "zn/net/udp_server.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif
/** 关键帧请求最小间隔，避免 PLI/NACK miss 抖动放大。 */
#define ZMS_WEBRTC_PLI_MIN_INTERVAL_MS 800u
/** TWCC 驱动的发送 pacer：默认不限制，仅平滑 I 帧突发；丢包后 AIMD 降速。 */
#define ZMS_WEBRTC_PACER_DEFAULT_BPS 20000000u
#define ZMS_WEBRTC_PACER_MIN_BPS 2000000u
#define ZMS_WEBRTC_PACER_MAX_BPS 20000000u
#define ZMS_WEBRTC_PACER_BURST_MS 50u

static uint32_t webrtc_ipv4_be(const char *ip)
{
    struct in_addr addr;
    /* 解析失败或空串回落到 127.0.0.1（回环），用于 STUN XOR-MAPPED-ADDRESS 在
     * 拿不到对端真实 IP 时的兜底，避免吐出一个 0.0.0.0 让浏览器 ICE 失败。 */
    if (!ip || !ip[0]) {
        return htonl(0x7f000001u);
    }
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return htonl(0x7f000001u);
    }
    return addr.s_addr;
}

static void webrtc_send_udp(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    if (!s || !data || len == 0) {
        return;
    }
    (void)zms_webrtc_session_send_udp(s, data, len);
}

void webrtc_play_gateway_cfg(const zms_webrtc_session *s, zms_webrtc_gateway_ingest_cfg *cfg)
{
    if (!s || !cfg) {
        return;
    }
    zms_webrtc_gateway_ingest_cfg_defaults(cfg);
    cfg->wire_video_pt = s->video_pt;
    cfg->wire_audio_pt = s->audio_pt;
    cfg->answer_has_video = s->answer_has_video;
    cfg->answer_has_audio = s->answer_has_audio;
}
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
static struct zms_webrtc_srtp *g_play_srtp_verify;
static char g_play_srtp_verify_id[ZMS_WEBRTC_SESSION_ID_LEN];
static void webrtc_play_srtp_verify_reset(void)
{
    zms_webrtc_srtp_destroy(g_play_srtp_verify);
    g_play_srtp_verify = NULL;
    g_play_srtp_verify_id[0] = '\0';
}
/** 自测 protect→unprotect（服务端写密钥，同 DTLS server 上 SRS send_key）。 */
void webrtc_play_srtp_roundtrip_check(zms_webrtc_session *s, const uint8_t *plain,
                                             size_t plain_len, size_t prot_len)
{
    uint8_t server_key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t server_salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    size_t check_len;
    static unsigned tests;
    if (!s || !s->dtls || !plain || plain_len == 0 || prot_len == 0 || tests >= 4u) {
        return;
    }
    if (!g_play_srtp_verify ||
        strncmp(g_play_srtp_verify_id, s->id, sizeof(g_play_srtp_verify_id)) != 0) {
        webrtc_play_srtp_verify_reset();
        g_play_srtp_verify = zms_webrtc_srtp_create();
        if (!g_play_srtp_verify ||
            zms_webrtc_dtls_export_server_srtp(s->dtls, server_key, server_salt) != 0) {
            return;
        }
        if (zms_webrtc_srtp_init_recv(g_play_srtp_verify, server_key, server_salt) != 0) {
            return;
        }
        if (zms_webrtc_session_io_buf_ensure(s) == 0) {
            zms_webrtc_srtp_bind_scratch(g_play_srtp_verify, s->io_buf, s->io_cap);
        }
        strncpy(g_play_srtp_verify_id, s->id, sizeof(g_play_srtp_verify_id) - 1);
    }
    if (prot_len > ZMS_WEBRTC_PLAY_RTP_SLOT_MAX || !s->io_buf) {
        return;
    }
    {
        uint8_t prot_tmp[ZMS_WEBRTC_PLAY_RTP_SLOT_MAX];
        memcpy(prot_tmp, s->io_buf, prot_len);
        check_len = prot_len;
        if (zms_webrtc_srtp_unprotect(g_play_srtp_verify, prot_tmp, &check_len) != 0) {
            ++tests;
            zn_warn(
                "[webrtc] play srtp selftest fail id=%s step=unprotect prot=%zu (send continues)",
                s->id, prot_len);
            return;
        }
        if (check_len != plain_len || memcmp(prot_tmp, plain, plain_len) != 0) {
            ++tests;
            zn_warn("[webrtc] play srtp selftest fail id=%s step=mismatch plain=%zu got=%zu",
                     s->id, plain_len, check_len);
            return;
        }
    }
    ++tests;
    zn_info("[webrtc] play srtp selftest ok id=%s pt=%u seq=%u ssrc=%u plain=%zu prot=%zu", s->id,
             (unsigned)zms_rtp_payload_type(plain, plain_len),
             (unsigned)(((uint16_t)plain[2] << 8) | plain[3]),
             (unsigned)(((uint32_t)plain[8] << 24) | ((uint32_t)plain[9] << 16) |
                        ((uint32_t)plain[10] << 8) | plain[11]),
             plain_len, prot_len);
}
#else
static void webrtc_play_srtp_verify_reset(void) {}
#endif
static void webrtc_on_pli(struct zms_webrtc_session *s)
{
    uint64_t now_ms;

    if (!s || !s->play.readers.gop) {
        return;
    }
    /* PLI（画面丢失，浏览器要求关键帧）节流：800ms 内重复请求直接计数丢弃，
     * 否则拥塞时浏览器疯狂发 PLI，每次 seek 关键帧反而加重拥塞。允许时把读
     * 者跳到 GOP 关键帧并让 muxer 快照新 GOP，从 IDR 重发。 */
    now_ms = zn_monotonic_ms();
    if (s->rt.pli_last_ms != 0 && now_ms - s->rt.pli_last_ms < ZMS_WEBRTC_PLI_MIN_INTERVAL_MS) {
        ++s->rt.pli_suppressed;
        return;
    }
    s->rt.pli_last_ms = now_ms;
    ++s->rt.pli_count;
    zms_gop_reader_seek_gop_key(s->play.readers.gop);
    if (s->play_rtp_muxer) {
        zms_egress_live_snap_gop(s->play.readers.gop, s->play_rtp_muxer);
    }
}

/** 依据 TWCC 反馈做简单 AIMD：丢包升高时降速，干净反馈缓慢回升。 */
static void webrtc_twcc_update_rate(zms_webrtc_session *s, uint32_t loss_permille)
{
    uint32_t ewma;
    uint64_t now_ms;

    if (!s || s->rt.twcc_ext_id <= 0) {
        return;
    }
    /* 丢包率做 1/8 权重的 EWMA，滤掉单次抖动。乘性降（×0.88）/ 加性增（×1.04）
     * 是 AIMD 的核心：降得快、升得慢，避免拥塞震荡。降/增各有最短间隔
     * （400ms/250ms）与窗口（ewma 阈值 + 队列等待），防止过于频繁调整。 */
    ewma = (s->rt.twcc_loss_ewma * 7u + loss_permille) / 8u;
    s->rt.twcc_loss_ewma = ewma;
    if (s->rt.send_bps == 0) {
        s->rt.send_bps = ZMS_WEBRTC_PACER_DEFAULT_BPS;
    }
    now_ms = zn_monotonic_ms();
    /* 丢包或发送队列等待升高都视为拥塞；降速后保留 400ms 观察窗口。 */
    if ((ewma >= 80u || s->rt.adapt_wait_ewma_ms > 180u) &&
        (s->rt.adapt_last_dec_ms == 0 || now_ms - s->rt.adapt_last_dec_ms >= 400u)) {
        uint32_t next = (uint32_t)((uint64_t)s->rt.send_bps * 88u / 100u);
        s->rt.send_bps = next < ZMS_WEBRTC_PACER_MIN_BPS ? ZMS_WEBRTC_PACER_MIN_BPS : next;
        s->rt.adapt_last_dec_ms = now_ms;
    } else if (ewma <= 10u && s->rt.adapt_wait_ewma_ms < 60u &&
               (s->rt.adapt_last_inc_ms == 0 || now_ms - s->rt.adapt_last_inc_ms >= 250u)) {
        uint64_t next = (uint64_t)s->rt.send_bps * 104u / 100u;
        s->rt.send_bps = next > ZMS_WEBRTC_PACER_MAX_BPS ? ZMS_WEBRTC_PACER_MAX_BPS : (uint32_t)next;
        s->rt.adapt_last_inc_ms = now_ms;
    }
}

static void webrtc_on_rtcp_block(const uint8_t *block, size_t block_len, void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    int budget = 64;
    int missed = 0;
    size_t off;

    if (!s || !block || block_len == 0) {
        return;
    }
    /* 逐个复合包分派：PLI→关键帧；TWCC→更新码率；RR(201)→RTT/丢包统计；
     * 其余 NACK 类→按 FCI 列表重传。budget=64 限制单次重传条数，避免一个
     * 恶意/拥塞反馈把 poller 卡住。 */
    if (zms_webrtc_rtcp_is_psfb_pli(block, block_len)) {
        webrtc_on_pli(s);
        return;
    }
    if (zms_webrtc_rtcp_is_rtpfb_twcc(block, block_len)) {
        zms_webrtc_twcc_summary tw;

        if (zms_webrtc_rtcp_parse_twcc(block, block_len, &tw) == 0) {
            ++s->rt.twcc_fb_pkts;
            s->rt.twcc_received += tw.received;
            s->rt.twcc_lost += tw.lost;
            s->rt.twcc_recovered += tw.recovered;
            if (tw.parsed > 0) {
                s->rt.twcc_loss_permille = (uint32_t)((tw.lost * 1000u) / tw.parsed);
                webrtc_twcc_update_rate(s, s->rt.twcc_loss_permille);
            }
        }
        return;
    }
    if (block_len >= 32 && block[1] == 201) {
        uint32_t fraction_lost = 0;
        uint32_t jitter = 0;
        uint32_t lsr = 0;
        uint32_t dlsr = 0;

        if (zms_webrtc_rtcp_parse_rr(block, block_len, &fraction_lost, &jitter, &lsr, &dlsr) == 0) {
            uint64_t now_ms = zn_wall_ms();
            uint32_t ntp_sec = (uint32_t)(now_ms / 1000u + 2208988800ULL);
            uint32_t ntp_frac = (uint32_t)(((now_ms % 1000u) * 65536u) / 1000u);
            uint32_t now_ntp_mid = (ntp_sec << 16) | ntp_frac;

            ++s->rt.rtcp_rr_pkts;
            s->rt.rtcp_fraction_lost = fraction_lost;
            s->rt.rtcp_jitter = jitter;
            if (lsr != 0 && dlsr != 0) {
                int64_t rtt = ((int64_t)(now_ntp_mid - lsr) - (int64_t)dlsr) * 1000 / 65536;
                if (rtt >= 0 && rtt < 60000) {
                    s->rt.rtcp_rtt_ms = rtt;
                }
            }
        }
        return;
    }
    if (!zms_webrtc_rtcp_is_rtpfb_nack(block, block_len) || block_len < 16) {
        return;
    }
    /* RTPFB NACK: 12 字节头 + FCI(PID, BLP) 列表。 */
    for (off = 12; off + 4 <= block_len && budget > 0; off += 4) {
        uint16_t pid = (uint16_t)(((uint16_t)block[off] << 8) | block[off + 1]);
        /* BLP 是位图：第 i 位表示 pid+i+1 也丢失，一次性纳补多个连续丢包。 */
        uint16_t blp = (uint16_t)(((uint16_t)block[off + 2] << 8) | block[off + 3]);
        int bit;

        if (webrtc_play_nack_retransmit(s, pid) != 0) {
            missed = 1;
        }
        --budget;
        for (bit = 0; bit < 16 && budget > 0; ++bit) {
            if ((blp & (uint16_t)(1u << bit)) == 0) {
                continue;
            }
            if (webrtc_play_nack_retransmit(s, (uint16_t)(pid + bit + 1)) != 0) {
                missed = 1;
            }
            --budget;
        }
    }
    if (missed) {
        /* 缓存没有命中时退回关键帧请求，避免等待重传链。 */
        webrtc_on_pli(s);
    }
    if (budget == 0) {
        zn_warn("[webrtc] nack retransmit budget exhausted id=%s", s->id);
    }
}

static int webrtc_play_send_srtcp_sr(zms_webrtc_session *s, int send_video, int send_audio)
{
    const zms_rtp_muxer_stats *st;
    const zms_egress_clock *clk;
    uint8_t rtcp[64];
    size_t n;
    uint32_t ntp_sec = 0;
    uint32_t ntp_frac = 0;
    uint32_t vhz = 90000;
    uint32_t ahz = 48000;
    struct zms_webrtc_srtp *srtp;
    int sent = 0;
    /* 用 muxer 统计里的“最后发出 RTP 时间戳 + 包数/字节数”构造 SR，音视频各
     * 一条。SR 必须先 SRTCP 加密再发（浏览器只解密后的 SR 才认）。任一轨发成功
     * 即返回 1。 */
    if (!s || !s->play_rtp_muxer || !s->ice_host.peer_known) {
        return 0;
    }
    st = zms_rtp_muxer_get_stats(s->play_rtp_muxer);
    clk = zms_rtp_muxer_play_clock(s->play_rtp_muxer);
    if (!st) {
        return 0;
    }
    if (send_video && s->source && s->source->has_video && st->video_pkt_count > 0) {
        if (clk) {
            zms_egress_clock_sr_ntp(clk, st->video_last_rtp_ts, vhz, &ntp_sec, &ntp_frac);
        }
        n = zms_webrtc_rtcp_build_sr(rtcp, sizeof(rtcp), 1, ntp_sec, ntp_frac,
                                     st->video_last_rtp_ts, st->video_pkt_count,
                                     st->video_octet_count);
        srtp = s->video_srtp;
        if (n && srtp && zms_webrtc_srtcp_protect(srtp, rtcp, &n, sizeof(rtcp)) == 0 &&
            zms_webrtc_session_send_udp(s, rtcp, n) == 0) {
            sent = 1;
        } else if (n && srtp) {
            zn_warn("[webrtc] srtcp video SR fail id=%s", s->id);
        }
    }
    if (send_audio && s->source && s->source->has_audio && s->answer_has_audio &&
        st->audio_pkt_count > 0) {
        if (clk) {
            zms_egress_clock_sr_ntp(clk, st->audio_last_rtp_ts, ahz, &ntp_sec, &ntp_frac);
        }
        n = zms_webrtc_rtcp_build_sr(rtcp, sizeof(rtcp), 2, ntp_sec, ntp_frac,
                                     st->audio_last_rtp_ts, st->audio_pkt_count,
                                     st->audio_octet_count);
        srtp = s->audio_srtp;
        if (n && srtp && zms_webrtc_srtcp_protect(srtp, rtcp, &n, sizeof(rtcp)) == 0 &&
            zms_webrtc_session_send_udp(s, rtcp, n) == 0) {
            sent = 1;
        } else if (n && srtp) {
            zn_warn("[webrtc] srtcp audio SR fail id=%s", s->id);
        }
    }
    return sent;
}
/** 对齐 RTSP rtsp_play_try_rtcp_boot：浏览器首 RTP 后需 SRTCP SR。 */
void webrtc_play_try_rtcp_boot(zms_webrtc_session *s)
{
    const zms_rtp_muxer_stats *st;
    int send_v = 0;
    int send_a = 0;
    /* 只在对应轨“已发过 RTP 且尚未发过 SR”时才补一次 SR：SR 携带 NTP↔RTP
     * 时钟映射，浏览器据此对齐音视频与做抖动计算；太早发（还没有 RTP 时间戳）
     * 没有意义。发成功后置已发标志，避免每帧都重复发。 */
    if (!s || !s->play_rtp_muxer || !s->source) {
        return;
    }
    st = zms_rtp_muxer_get_stats(s->play_rtp_muxer);
    if (!st) {
        return;
    }
    if (s->source->has_video && !s->rt.rtcp_video_sr_sent && st->video_pkt_count > 0) {
        send_v = 1;
    }
    if (s->source->has_audio && s->answer_has_audio && !s->rt.rtcp_audio_sr_sent &&
        st->audio_pkt_count > 0) {
        send_a = 1;
    }
    if (!send_v && !send_a) {
        return;
    }
    if (webrtc_play_send_srtcp_sr(s, send_v, send_a)) {
        if (send_v) {
            s->rt.rtcp_video_sr_sent = 1;
        }
        if (send_a) {
            s->rt.rtcp_audio_sr_sent = 1;
        }
        zn_info("[webrtc] play srtcp boot id=%s video=%d audio=%d v_pkts=%u", s->id, send_v,
                 send_a, st->video_pkt_count);
    }
}

static void webrtc_play_send_rtcp_sr(zms_webrtc_session *s)
{
    if (!s || !s->play_rtp_muxer) {
        return;
    }
    /* 周期性 SR：音视频都尝试；实际是否发由 send_srtcp_sr 内部按“已发过 RTP”
     * 判断，没出过流的轨不会发。 */
    (void)webrtc_play_send_srtcp_sr(s, 1, 1);
}



/** 低延迟策略：拥塞时清空已排队 RTP，等待关键帧恢复。 */


void webrtc_play_diag(zms_webrtc_session *s, const char *tag, const char *detail)
{
#if defined(ZMS_WEBRTC_PLAY_DIAG)
    if (!s || !tag) {
        return;
    }
    zn_info("[webrtc] diag id=%s tid=%llu poller=%d q=%u tag=%s %s", s->id,
             (unsigned long long)zb_thread_self_id(),
             s->poller ? zn_poller_is_current_thread(s->poller) : -1, s->rt.q_n, tag,
             detail ? detail : "");
#else
    (void)s;
    (void)tag;
    (void)detail;
#endif
}

static void webrtc_play_trace(zms_webrtc_session *s, const char *phase, unsigned extra)
{
#if defined(ZMS_WEBRTC_PLAY_TRACE)
    if (!s || !phase) {
        return;
    }
    zn_info("[webrtc] trace id=%s step=%u phase=%s extra=%u q=%u", s->id, ++s->play_trace_seq,
             phase, extra, s->rt.q_n);
#else
    (void)s;
    (void)phase;
    (void)extra;
#endif
}


/**
 * 帧级码率门控：仅在 TWCC pacer 启用且最近 1s 实际码率超过目标 90% 时，
 * 丢弃非关键帧整帧；丢帧后等待下一个关键帧并请求 GOP 关键帧重发。
 */








/** 令牌桶 pacer：按 send_bps 放行，允许 50ms 突发；未协商 TWCC 时直接放行。 */


static void webrtc_play_load_h264_ps(zms_webrtc_session *s)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t vcfg_len = 0;
    const uint8_t *vcfg;
    if (!s) {
        return;
    }
    /* 起点先清零：若本次取不到参数集，play_h264_sps_len==0 会让 STAP-A 注入
     * 直接跳过，避免用上一次（过期）的 SPS/PPS 拼包。优先从源 config 取，
     * 回落到 gop_queue 的视频 config。 */
    s->play_h264_sps_len = 0;
    s->play_h264_pps_len = 0;
    s->play_h264_seq_bias = 0;
    if (!s->source || s->video_codec != ZMS_CODEC_H264) {
        return;
    }
    vcfg = zms_media_source_video_config(s->source, &vcfg_len);
    if (vcfg && vcfg_len &&
        zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, &sps, &sps_len, &pps, &pps_len)) {
        goto store;
    }
    if (s->source->gop_queue) {
        vcfg = zms_gop_queue_video_config(s->source->gop_queue, &vcfg_len);
        if (vcfg && vcfg_len &&
            zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, &sps, &sps_len, &pps, &pps_len)) {
            goto store;
        }
    }
    return;
store:
    if (!sps || !sps_len || sps_len > sizeof(s->play_h264_sps) || !pps || !pps_len ||
        pps_len > sizeof(s->play_h264_pps)) {
        return;
    }
    memcpy(s->play_h264_sps, sps, sps_len);
    memcpy(s->play_h264_pps, pps, pps_len);
    s->play_h264_sps_len = sps_len;
    s->play_h264_pps_len = pps_len;
    zn_info("[webrtc] play h264 stap ps id=%s sps=%zu pps=%zu", s->id, sps_len, pps_len);
}

static int webrtc_rtp_is_h264_idr_fu_start(const uint8_t *rtp, size_t len)
{
    size_t off;
    uint8_t fu_ind;
    uint8_t fu_hdr;
    if (!rtp || len < 14) {
        return 0;
    }
    off = zms_rtp_payload_offset(rtp, len);
    if (off == 0 || off + 2 > len) {
        return 0;
    }
    fu_ind = rtp[off];
    fu_hdr = rtp[off + 1];
    if ((fu_ind & 0x1fu) != 28u) {
        return 0;
    }
    if ((fu_hdr & 0x80u) == 0) {
        return 0;
    }
    if ((fu_hdr & 0x1fu) != 5u) {
        return 0;
    }
    return 1;
}

static void webrtc_rtp_bump_seq(uint8_t *rtp, size_t len, uint16_t bias)
{
    uint16_t seq;
    if (!rtp || len < 4 || bias == 0) {
        return;
    }
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    seq = (uint16_t)(seq + bias);
    rtp[2] = (uint8_t)(seq >> 8);
    rtp[3] = (uint8_t)(seq & 0xffu);
}

static int webrtc_play_build_h264_stap_a(const zms_webrtc_session *s, const uint8_t *tpl,
                                         size_t tpl_len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t pay_len;
    uint8_t stap_hdr;
    size_t pos;
    if (!s || !tpl || tpl_len < 12 || !out || !out_len || s->play_h264_sps_len == 0 ||
        s->play_h264_pps_len == 0) {
        return -1;
    }
    pay_len = 1 + 2 + s->play_h264_sps_len + 2 + s->play_h264_pps_len;
    if (12 + pay_len > cap) {
        return -1;
    }
    memcpy(out, tpl, 12);
    webrtc_rtp_bump_seq(out, 12, s->play_h264_seq_bias);
    out[1] = (uint8_t)((out[1] & 0x80u) | (tpl[1] & 0x7fu)); /* STAP-A 上 marker=0 */
    stap_hdr = (uint8_t)((s->play_h264_sps[0] & 0x60u) | 24u);
    pos = 12;
    out[pos++] = stap_hdr;
    out[pos++] = (uint8_t)(s->play_h264_sps_len >> 8);
    out[pos++] = (uint8_t)(s->play_h264_sps_len);
    memcpy(out + pos, s->play_h264_sps, s->play_h264_sps_len);
    pos += s->play_h264_sps_len;
    out[pos++] = (uint8_t)(s->play_h264_pps_len >> 8);
    out[pos++] = (uint8_t)(s->play_h264_pps_len);
    memcpy(out + pos, s->play_h264_pps, s->play_h264_pps_len);
    pos += s->play_h264_pps_len;
    *out_len = pos;
    return 0;
}

static void webrtc_on_mux_rtp(zms_rtp_mux_track track, const uint8_t *rtp, size_t len, void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    uint8_t wire[ZMS_WEBRTC_PLAY_RTP_SLOT_MAX];
    size_t wire_len = len;
    uint8_t stap[ZMS_WEBRTC_PLAY_H264_STAP_MAX];
    size_t stap_len = 0;
    if (!s || !rtp || len < 12 || len > sizeof(wire)) {
        return;
    }
    /* 每个 H.264 IDR 帧首包前额外插一个 STAP-A（SPS+PPS）：浏览器/newer 解码器
     * 期望关键帧自带参数集，只发带内参数集不够可靠。插入后给后续真实包统一
     * 加 seq_bias，保证序号连续不跳。 */
    if (track == ZMS_RTP_MUX_TRACK_VIDEO && s->video_codec == ZMS_CODEC_H264 &&
        s->play_h264_sps_len > 0 && webrtc_rtp_is_h264_idr_fu_start(rtp, len)) {
        if (webrtc_play_build_h264_stap_a(s, rtp, len, stap, sizeof(stap), &stap_len) == 0) {
            static unsigned stap_log;
            (void)webrtc_play_rtp_q_push(s, track, stap, stap_len);
            ++s->play_h264_seq_bias;
            if (stap_log < 8) {
                ++stap_log;
                zn_info("[webrtc] play h264 stap-a id=%s seq=%u ts=%u bias=%u len=%zu", s->id,
                         (unsigned)(((uint16_t)stap[2] << 8) | stap[3]),
                         (unsigned)(((uint32_t)stap[4] << 24) | ((uint32_t)stap[5] << 16) |
                                    ((uint32_t)stap[6] << 8) | stap[7]),
                         (unsigned)s->play_h264_seq_bias, stap_len);
            }
        }
    }
    memcpy(wire, rtp, len);
    if (track == ZMS_RTP_MUX_TRACK_VIDEO && s->play_h264_seq_bias > 0) {
        webrtc_rtp_bump_seq(wire, wire_len, s->play_h264_seq_bias);
    }
    if (track == ZMS_RTP_MUX_TRACK_AUDIO) {
        /* 音频低延迟优先：绕过视频 RTP 队列/pacer/丢帧策略，直接 SRTP 发送。 */
        if (webrtc_play_send_rtp_item(s, track, wire, wire_len, 0) == 0) {
            webrtc_play_try_rtcp_boot(s);
        }
        return;
    }
    (void)webrtc_play_rtp_q_push(s, track, wire, wire_len);
}

static void webrtc_play_arm_mux(zms_webrtc_session *s)
{
    zms_rtp_muxer_opts opts;
    zms_rtp_play_mux_opts ocfg;
    zms_rtp_play_mux_handle opened;
    if (!s || s->play_rtp_muxer) {
        return;
    }
    /* 视频时钟恒为 90kHz（RTP 视频标准）；音频时钟随 codec（Opus 48k/G711 8k）。
     * video_ssrc/audio_ssrc 固定 1/2，seq 从 1 起，保证每个会话的 RTP 头稳定。
     * pt 用协商得到的 payload type。 */
    memset(&opts, 0, sizeof(opts));
    opts.video_clock_hz = 90000;
    opts.audio_clock_hz = 48000;
    opts.audio_rate = 48000;
    opts.audio_codec = ZMS_CODEC_INVALID;
    if (s->answer_has_audio) {
        if (s->audio_codec == ZMS_CODEC_OPUS || s->audio_codec == ZMS_CODEC_G711A ||
            s->audio_codec == ZMS_CODEC_G711U) {
            opts.audio_codec = s->audio_codec;
        } else if (s->audio_codec != ZMS_CODEC_INVALID) {
            /* 非 Opus/G711 不送音轨，避免 AAC 进 WebRTC mux */
            /* WebRTC 没有 AAC 的强制协商（除非用非标准红线），把 AAC 塞进
             * RTP 会让浏览器无从解码；宁可丢音频也不发浏览器解不了的轨。 */
            zn_info("[webrtc] mux drop unsupported audio=%s id=%s",
                     zms_codec_name(s->audio_codec), s->id);
            opts.audio_codec = ZMS_CODEC_INVALID;
        }
    }
    /* Opus 采样率取协商值（常见 48k）；G711 固定 8k。audio_clock_hz 与采样率
     * 一致，供 RTP 时间戳换算。 */
    if (opts.audio_codec == ZMS_CODEC_OPUS) {
        opts.audio_rate = s->audio_rate > 0 ? s->audio_rate : 48000;
        opts.audio_clock_hz = (uint32_t)opts.audio_rate;
    } else if (opts.audio_codec == ZMS_CODEC_G711A || opts.audio_codec == ZMS_CODEC_G711U) {
        opts.audio_rate = 8000;
        opts.audio_clock_hz = 8000;
    }
    opts.video_pt = s->video_pt ? s->video_pt : 96;
    opts.audio_pt = s->answer_has_audio ? s->audio_pt : 97;
    opts.video_ssrc = 1;
    opts.audio_ssrc = 2;
    opts.video_seq = 1;
    opts.audio_seq = 1;
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.mux_opts = opts;
    ocfg.reader = &s->play;
    ocfg.on_rtp = webrtc_on_mux_rtp;
    ocfg.user = s;
    if (zms_rtp_play_mux_create(&opened, &ocfg) != ZN_OK || !opened.mux) {
        return;
    }
    s->egress_pipe = opened.egress;
    s->play_rtp_muxer = opened.mux;
    zms_rtp_muxer_arm_play(s->play_rtp_muxer);
    (void)zms_rtp_play_bootstrap_live(s->play_rtp_muxer, s->source, s->play.readers.gop, s->session_no);
    if (s->play.readers.gop) {
        zms_gop_reader_seek_live_idr(s->play.readers.gop);
        if (s->play_rtp_muxer) {
            zms_egress_live_snap_gop(s->play.readers.gop, s->play_rtp_muxer);
        }
    }
    zn_info("[webrtc] media mux ready id=%s", s->id);
}

static void webrtc_play_pump_work_impl(void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    zms_egress_live_state live;
    int frame_budget;
    if (!s) {
        return;
    }
    s->play_pump_armed = 0;
    if (!s->play_rtp_muxer || !s->play.readers.gop) {
        return;
    }
    /* 与 zms_rtp_play_pump_run 同模式：单 tick 内 egress pump 再 SRTP/UDP flush。 */
    frame_budget =
        s->live_catchup_done ? (int)ZMS_RTP_PLAY_FRAME_BUDGET_LIVE : (int)ZMS_RTP_PLAY_FRAME_BUDGET;
    if (s->rt.q_n + 16u >= ZMS_WEBRTC_PLAY_RTPQ_CAP) {
        return;
    }
    memset(&live, 0, sizeof(live));
    live.live_catchup_done = &s->live_catchup_done;
    live.pump_budget_cap = 0;
    (void)zms_egress_pipeline_pump_live(s->egress_pipe, frame_budget, 500, s->session_no, &live);
    if (s->rt.q_n > 0) {
        unsigned flush_left = s->live_catchup_done ? (unsigned)ZMS_WEBRTC_PLAY_RTP_FLUSH
                                                   : (unsigned)ZMS_RTP_PLAY_RTP_FLUSH;
        while (s->rt.q_n > 0 && flush_left > 0) {
            unsigned sent = webrtc_play_rtp_q_flush(s, flush_left);
            if (sent == 0) {
                break;
            }
            if (sent >= flush_left) {
                break;
            }
            flush_left -= sent;
        }
    }
    /* 队列积压严重（adapt_wait>500ms）时，等满 1s 就整体清空队列并请求关键帧：
     * 与其慢慢发积压的旧帧让延迟越来越大，不如丢弃重来，代价是一次短黑屏。
     * 中等积压（>250ms）则先温和降码率，给 pacer 一个调整机会。 */
    if (s->rt.send_bps > 0 && s->rt.adapt_wait_ewma_ms > 500u) {
        uint64_t now_ms = zn_monotonic_ms();

        if (s->rt.adapt_last_reset_ms == 0 || now_ms - s->rt.adapt_last_reset_ms >= 1000u) {
            size_t dropped = webrtc_play_rtp_q_clear(s);

            s->rt.adapt_last_reset_ms = now_ms;
            ++s->rt.adapt_resets;
            s->rt.adapt_wait_ewma_ms = 0;
            s->rt.adapt_need_key = 1;
            zn_warn("[webrtc] adapt queue reset id=%s dropped=%zu q=%u", s->id, dropped,
                     s->rt.q_n);
        }
    } else if (s->rt.send_bps > 0 && s->rt.adapt_wait_ewma_ms > 250u) {
        uint64_t now_ms = zn_monotonic_ms();

        if (s->rt.adapt_last_dec_ms == 0 || now_ms - s->rt.adapt_last_dec_ms >= 500u) {
            uint32_t next = (uint32_t)((uint64_t)s->rt.send_bps * 90u / 100u);
            s->rt.send_bps = next < ZMS_WEBRTC_PACER_MIN_BPS ? ZMS_WEBRTC_PACER_MIN_BPS : next;
            s->rt.adapt_last_dec_ms = now_ms;
            s->rt.adapt_dropping = 1;
            zn_info("[webrtc] adapt queue-delay id=%s wait=%llums bps=%u", s->id,
                     (unsigned long long)s->rt.adapt_wait_ewma_ms, s->rt.send_bps);
        }
    }
    if (s->rt.adapt_need_key) {
        s->rt.adapt_need_key = 0;
        webrtc_on_pli(s);
    }
    if (++s->rt.rtcp_tick >= 250) {
        /* 约每 250 个 tick 周期补发一次 SR（不依赖 try_rtcp_boot 的一次性），
         * 保持浏览器侧时钟映射新鲜。 */
        s->rt.rtcp_tick = 0;
        webrtc_play_send_rtcp_sr(s);
    }
}

static void webrtc_play_pump_work(void *user)
{
#if defined(_WIN32)
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    __try {
        webrtc_play_pump_work_impl(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s) {
            s->play_pump_armed = 0;
        }
        zn_warn("[webrtc] FAULT pump id=%s code=0x%08x step=%u q=%u", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), s ? s->play_trace_seq : 0u,
                 s ? s->rt.q_n : 0u);
    }
#else
    webrtc_play_pump_work_impl(user);
#endif
}

static uint64_t webrtc_play_pump_timer_impl(void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    if (!s || !s->poller) {
        return 0;
    }
    webrtc_play_pump_work(s);
    /* 队列里有积压就用 1ms 尽快续跑排空；空闲时 5ms 一次，降低空转开销。 */
    return s->rt.q_n > 0 ? 1 : 5;
}

static uint64_t webrtc_play_pump_timer(void *user)
{
#if defined(_WIN32)
    zms_webrtc_session *s = (zms_webrtc_session *)user;
    __try {
        return webrtc_play_pump_timer_impl(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        zn_warn("[webrtc] FAULT id=%s code=0x%08x step=%u q=%u", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), s ? s->play_trace_seq : 0u,
                 s ? s->rt.q_n : 0u);
        return 5;
    }
#else
    return webrtc_play_pump_timer_impl(user);
#endif
}

static void webrtc_play_arm_pump_task(void *user)
{
    zms_webrtc_session *s = (zms_webrtc_session *)user;

    if (!s || !s->poller || s->pump_timer) {
        return;
    }
    s->pump_timer = zn_poller_do_delay(s->poller, 0, webrtc_play_pump_timer, s);
}

static void webrtc_play_start_media(zms_webrtc_session *s)
{
    uint8_t key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    uint8_t rkey[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t rsalt[ZMS_WEBRTC_SRTP_SALT_LEN];
    /* media_started 幂等：DTLS 完成与首帧可能都触发到这里，只跑一次完整
     * 密钥导出 + muxer 装配。非 PLAY 模式（PUBLISH）不在此启动出流。 */
    if (!s || s->media_started || s->mode != ZMS_WEBRTC_SESSION_PLAY) {
        return;
    }
    if (s->rt.twcc_ext_id > 0) {
        /* 压测/联调：可用环境变量强制初始目标码率；正常路径等首个 TWCC 反馈再启用 pacer。 */
        const char *env = getenv("ZMS_WEBRTC_TARGET_BPS");

        if (env && env[0]) {
            long v = strtol(env, NULL, 10);
            if (v >= (long)ZMS_WEBRTC_PACER_MIN_BPS && v <= (long)ZMS_WEBRTC_PACER_MAX_BPS) {
                s->rt.send_bps = (uint32_t)v;
                zn_info("[webrtc] adapt override id=%s target=%u", s->id, s->rt.send_bps);
            }
        }
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (!s->dtls) {
        return;
    }
    if (s->dtls_as_client) {
        /* 主动模式：本端发送用 client_write，接收反馈用 server_write。 */
        /* DTLS 角色决定哪套密钥用于发送：DTLS client 用 client write / server
         * read，DTLS server 反之。这里两种角色的 key 导出方向要正确对应，
         * 否则 SRTP 加密后浏览器解不开。 */
        if (zms_webrtc_dtls_export_client_srtp(s->dtls, key, salt) != 0 ||
            zms_webrtc_dtls_export_server_srtp(s->dtls, rkey, rsalt) != 0) {
            return;
        }
    } else {
        if (zms_webrtc_dtls_export_server_srtp(s->dtls, key, salt) != 0) {
            return;
        }
        if (zms_webrtc_dtls_export_client_srtp(s->dtls, rkey, rsalt) != 0) {
            zn_warn("[webrtc] export client srtp failed id=%s", s->id);
        }
    }
    if (!s->video_srtp) {
        s->video_srtp = zms_webrtc_srtp_create();
    }
    if (!s->audio_srtp) {
        s->audio_srtp = zms_webrtc_srtp_create();
    }
    if (!s->video_srtp_recv) {
        s->video_srtp_recv = zms_webrtc_srtp_create();
    }
    if (!s->audio_srtp_recv) {
        s->audio_srtp_recv = zms_webrtc_srtp_create();
    }
    if (!s->video_srtp || !s->audio_srtp || !s->video_srtp_recv || !s->audio_srtp_recv) {
        return;
    }
    zms_webrtc_srtp_init_send(s->video_srtp, key, salt);
    zms_webrtc_srtp_init_send(s->audio_srtp, key, salt);
    zms_webrtc_srtp_init_recv(s->video_srtp_recv, rkey, rsalt);
    zms_webrtc_srtp_init_recv(s->audio_srtp_recv, rkey, rsalt);
#else
    (void)key;
    (void)salt;
    (void)rkey;
    (void)rsalt;
    return;
#endif
    webrtc_play_rtp_q_free(s);
    if (webrtc_play_rtp_q_ensure(s) != 0) {
        return;
    }
    if (zms_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    zms_webrtc_srtp_bind_scratch(s->video_srtp, s->io_buf, s->io_cap);
    zms_webrtc_srtp_bind_scratch(s->audio_srtp, s->io_buf, s->io_cap);
    zms_webrtc_srtp_bind_scratch(s->video_srtp_recv, s->io_buf, s->io_cap);
    zms_webrtc_srtp_bind_scratch(s->audio_srtp_recv, s->io_buf, s->io_cap);
    webrtc_play_arm_mux(s);
    if (!s->play_rtp_muxer) {
        return;
    }
    webrtc_play_load_h264_ps(s);
    s->media_started = 1;
    s->live_catchup_done = 1;
    if (s->poller && !s->pump_timer) {
        /* 本函数可能运行在 timer flush 回调里（timer_mtx 已持有），不能直接 do_delay。 */
        if (zn_poller_async(s->poller, webrtc_play_arm_pump_task, s, 0) != ZN_OK) {
            zn_warn("[webrtc] pump arm async failed id=%s", s->id);
        }
    }
}

static uint64_t webrtc_play_deferred_start(void *user)
{
    webrtc_play_start_media((zms_webrtc_session *)user);
    return 0;
}

static zn_err_t webrtc_handle_stun(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    uint8_t reply[128];
    size_t reply_len;
    uint32_t mapped_ip;
    if (!s->local_pwd[0]) {
        return ZN_ERR_INVALID;
    }
    /* 只应答 binding request，且带 ICE 密码校验（zms_webrtc_stun_binding_reply
     * 内部校验 integrity）；回包带上观察到的对端地址作为 XOR-MAPPED-ADDRESS，
     * 让浏览器得知自己的公网映射。 */
    if (!zms_webrtc_stun_is_binding_req(data, len)) {
        return ZN_ERR_INVALID;
    }
    mapped_ip = webrtc_ipv4_be(s->ice_host.peer_ip);
    reply_len = zms_webrtc_stun_binding_reply(data, len, reply, sizeof(reply), mapped_ip,
                                              s->ice_host.peer_port, s->local_pwd);
    if (reply_len == 0) {
        return ZN_ERR_INVALID;
    }
    webrtc_send_udp(s, reply, reply_len);
    if (s->mode == ZMS_WEBRTC_SESSION_PUBLISH) {
        zms_webrtc_session_try_dtls_client(s);
    }
    return ZN_OK;
}

static struct zms_webrtc_dtls *webrtc_session_get_dtls(zms_webrtc_session *s)
{
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (!s->dtls) {
        if (s->dtls_as_client) {
            s->dtls = zms_webrtc_dtls_create_client();
        } else {
            s->dtls = zms_webrtc_dtls_create();
        }
    }
#endif
    return s->dtls;
}

static zn_err_t webrtc_handle_dtls(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    uint8_t out[ZMS_WEBRTC_PLAY_DTLS_IO];
    size_t out_len = 0;
    int st;
#if !defined(ZMS_HAVE_WEBRTC_DTLS) || !ZMS_HAVE_WEBRTC_DTLS
    (void)data;
    (void)len;
    (void)out;
    return ZN_ERR_NOT_IMPL;
#else
    if (!s->dtls) {
        s->dtls = webrtc_session_get_dtls(s);
    }
    if (!s->dtls) {
        return ZN_ERR_STATE;
    }
    st = zms_webrtc_dtls_input(s->dtls, data, len, out, sizeof(out), &out_len);
    if (out_len > 0) {
        webrtc_send_udp(s, out, out_len);
    }
    if (st < 0) {
        return ZN_ERR_STATE;
    }
    if (st == 1 && !s->dtls_ready) {
        s->dtls_ready = 1;
        /* 握手完成：不能在本回调栈里直接 start_media（可能重入或持有 timer
         * 锁），改用 do_delay(0) 投到下个事件循环再启动出流。 */
        if (s->mode == ZMS_WEBRTC_SESSION_PLAY && s->poller) {
            (void)zn_poller_do_delay(s->poller, 0, webrtc_play_deferred_start, s);
        }
    }
    return ZN_OK;
#endif
}

zn_err_t zms_webrtc_play_on_stun_dtls(struct zms_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    if (!s || !pkt || len == 0) {
        return ZN_ERR_INVALID;
    }
    if (zms_webrtc_stun_is_binding_req(pkt, len)) {
        return webrtc_handle_stun(s, pkt, len);
    }
    if (zms_webrtc_packet_is_dtls(pkt, len)) {
        static unsigned dtls_log;
        if (dtls_log < 8) {
            ++dtls_log;
            zn_info("[webrtc] DTLS in id=%s len=%zu peer=%s:%u", s->id, len, s->ice_host.peer_ip,
                     (unsigned)s->ice_host.peer_port);
        }
        return webrtc_handle_dtls(s, pkt, len);
    }
    return ZN_ERR_INVALID;
}

static size_t webrtc_rtp_payload_offset(const uint8_t *rtp, size_t len)
{
    size_t off;

    if (!rtp || len < 12) {
        return 0;
    }
    off = 12u + 4u * (size_t)(rtp[0] & 0x0f);
    if (off > len) {
        return 0;
    }
    if (rtp[0] & 0x10) {
        if (off + 4 > len) {
            return 0;
        }
        off += 4u + 4u * (size_t)(((uint16_t)rtp[off + 2] << 8) | rtp[off + 3]);
    }
    return off <= len ? off : 0;
}

/** 处理浏览器对讲 RTP：交付消费者；无消费者时 G711 回环便于联调。 */
static void webrtc_play_on_talk_rtp(zms_webrtc_session *s, uint8_t *rtp, size_t len)
{
    size_t off;
    size_t plen;
    size_t pad = 0;
    uint8_t out[12 + 1500];
    size_t out_len;

    if (!s || !rtp || len < 12 || (rtp[0] >> 6) != 2) {
        return;
    }
    /* 只接受匹配 talk_pt 的包（版本必须 2）。从 CSRC/扩展头算 payload 起点，
     * 若 P 位（填充）置位则扣掉尾部填充字节。 */
    if ((rtp[1] & 0x7f) != s->talk_pt) {
        return;
    }
    off = webrtc_rtp_payload_offset(rtp, len);
    if (off == 0 || off >= len) {
        return;
    }
    plen = len - off;
    if (rtp[0] & 0x20) {
        pad = rtp[len - 1];
        if (pad >= plen) {
            return;
        }
        plen -= pad;
    }
    if (plen == 0) {
        return;
    }
    ++s->talk_rx_pkts;
    s->talk_rx_bytes += plen;
    if (s->talk_rx_pkts <= 3 || s->talk_rx_pkts % 500 == 0) {
        zn_info("[webrtc] talk rx id=%s codec=%s pt=%u len=%zu pkts=%llu", s->id,
                 zms_codec_name((zms_codec_id)s->talk_codec), (unsigned)s->talk_pt, plen,
                 (unsigned long long)s->talk_rx_pkts);
    }
    if (zms_webrtc_server_emit_talk_audio(s->app, s->stream, s->talk_codec, s->talk_rate,
                                           rtp + off, plen)) {
        return;
    }
    if (s->answer_has_audio ||
        (s->talk_codec != ZMS_CODEC_G711U && s->talk_codec != ZMS_CODEC_G711A)) {
        return;
    }
    if (plen + 12 > sizeof(out)) {
        return;
    }
    /* 无消费者时的 G711 回环：把收到的 payload 重新打成一个本端 RTP 包（固定
     * SSRC=2、seq/ts 自增），各自 SRTP 加密后发回浏览器，便于单独联调对讲链路
     * 而不需要真正的音频消费者。 */
    out[0] = 0x80;
    out[1] = (uint8_t)s->talk_pt;
    out[2] = (uint8_t)(s->talk_seq >> 8);
    out[3] = (uint8_t)(s->talk_seq & 0xff);
    out[4] = (uint8_t)(s->talk_ts >> 24);
    out[5] = (uint8_t)(s->talk_ts >> 16);
    out[6] = (uint8_t)(s->talk_ts >> 8);
    out[7] = (uint8_t)s->talk_ts;
    out[8] = 0;
    out[9] = 0;
    out[10] = 0;
    out[11] = 2;
    memcpy(out + 12, rtp + off, plen);
    out_len = plen + 12;
    ++s->talk_seq;
    s->talk_ts += (uint32_t)plen;
    if (s->audio_srtp && zms_webrtc_srtp_protect(s->audio_srtp, out, &out_len, sizeof(out)) == 0) {
        if (s->talk_seq <= 3) {
            zn_info("[webrtc] talk loopback id=%s len=%zu seq=%u", s->id, out_len,
                     (unsigned)(s->talk_seq - 1));
        }
        webrtc_send_udp(s, out, out_len);
    }
}

static void webrtc_play_dispatch_rtcp(zms_webrtc_session *s, const uint8_t *data, size_t len)
{
    uint8_t copy[ZMS_WEBRTC_PLAY_RTP_SLOT_MAX];

    if (!s || !data || len == 0) {
        return;
    }
    /* NACK 重传会复用 s->io_buf；解析前先拷出，避免边遍历边覆盖。 */
    if (len <= sizeof(copy)) {
        memcpy(copy, data, len);
        zms_webrtc_rtcp_for_each_compound(copy, len, webrtc_on_rtcp_block, s);
        return;
    }
    zms_webrtc_rtcp_for_each_compound(data, len, webrtc_on_rtcp_block, s);
}

void zms_webrtc_play_input(struct zms_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    zms_webrtc_gateway_pkt_kind kind;
    if (!s || !pkt || len == 0) {
        return;
    }
    if (zms_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    kind = zms_webrtc_gateway_classify(pkt, len);
    if (kind == ZMS_WEBRTC_GATEWAY_SRTCP) {
        /* SRTCP 先按 video 密钥试解，失败再按 audio 试解：两者共用同一对端
         * 端口，只能靠试解来区分是哪条轨的控制包。 */
        size_t rtcp_len = len;
        if (rtcp_len > s->io_cap || !s->io_buf) {
            return;
        }
        memcpy(s->io_buf, pkt, rtcp_len);
        if (s->video_srtp_recv &&
            zms_webrtc_srtcp_unprotect(s->video_srtp_recv, s->io_buf, &rtcp_len) == 0) {
            webrtc_play_dispatch_rtcp(s, s->io_buf, rtcp_len);
            return;
        }
        rtcp_len = len;
        memcpy(s->io_buf, pkt, rtcp_len);
        if (s->audio_srtp_recv &&
            zms_webrtc_srtcp_unprotect(s->audio_srtp_recv, s->io_buf, &rtcp_len) == 0) {
            webrtc_play_dispatch_rtcp(s, s->io_buf, rtcp_len);
            return;
        }
        return;
    }
    if (kind == ZMS_WEBRTC_GATEWAY_SRTP) {
        if (s->talk_enabled && s->audio_srtp_recv && len >= 12) {
            static unsigned srtp_in_log;
            size_t rtp_len = len;
            uint8_t pt = (uint8_t)(pkt[1] & 0x7f);
            int unprot;

            if (srtp_in_log < 6) {
                ++srtp_in_log;
                zn_info("[webrtc] srtp in id=%s pt=%u len=%zu talk_pt=%u", s->id,
                         (unsigned)pt, len, (unsigned)s->talk_pt);
            }
            if (pt == s->talk_pt && rtp_len <= s->io_cap) {
                memcpy(s->io_buf, pkt, rtp_len);
                unprot = zms_webrtc_srtp_unprotect(s->audio_srtp_recv, s->io_buf, &rtp_len);
                if (unprot == 0) {
                    webrtc_play_on_talk_rtp(s, s->io_buf, rtp_len);
                } else if (s->talk_rx_pkts == 0) {
                    zn_warn("[webrtc] talk srtp unprotect fail id=%s len=%zu", s->id, len);
                }
            }
        }
        return;
    }
    if (kind == ZMS_WEBRTC_GATEWAY_DTLS) {
        /* DTLS 记录层（握手/心跳）：交给 dtls 状态机，回包同样走 UDP。 */
        (void)webrtc_handle_dtls(s, pkt, len);
    }
}

void zms_webrtc_play_stop(struct zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    /* 停流：复位全局 SRTP 自测对象、停 pump 定时器、销毁 RTP 队列与 egress
     * 管线、销毁 DTLS 与四个 SRTP 上下文，并清掉“已发 SR/已起流”等标志，
     * 使会话可被重新拉起（例如重协商）。 */
    webrtc_play_srtp_verify_reset();
    s->play_pump_armed = 0;
    webrtc_play_rtp_q_free(s);
    if (s->pump_timer) {
        zn_poller_timer_cancel(s->pump_timer);
        s->pump_timer = NULL;
    }
    if (s->egress_pipe) {
        zms_rtp_play_mux_handle h = {s->egress_pipe, s->play_rtp_muxer};
        zms_rtp_play_mux_destroy(&h);
        s->egress_pipe = NULL;
        s->play_rtp_muxer = NULL;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (s->dtls) {
        zms_webrtc_dtls_destroy(s->dtls);
        s->dtls = NULL;
    }
    zms_webrtc_srtp_destroy(s->video_srtp);
    s->video_srtp = NULL;
    zms_webrtc_srtp_destroy(s->audio_srtp);
    s->audio_srtp = NULL;
    zms_webrtc_srtp_destroy(s->video_srtp_recv);
    s->video_srtp_recv = NULL;
    zms_webrtc_srtp_destroy(s->audio_srtp_recv);
    s->audio_srtp_recv = NULL;
#endif
    s->media_started = 0;
    s->dtls_ready = 0;
    s->rt.rtcp_video_sr_sent = 0;
    s->rt.rtcp_audio_sr_sent = 0;
}
