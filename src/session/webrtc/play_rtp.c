/**
 * @file play_rtp.c
 * @brief WebRTC 播放发送面：RTP 队列、NACK 缓存与重传、TWCC 驱动的节拍与帧级自适应。
 *
 * 发送流水：hub → rtp_muxer → webrtc_play_rtp_q_push（入队）→ webrtc_play_rtp_q_flush
 * （按 pacer 令牌出队）→ webrtc_play_send_rtp_item（PT 重映射 + TWCC 扩展 + SRTP）→
 * udp sendto。视频包额外进 NACK 缓存以便丢包重传。
 *
 * 面对“网络比源慢”的两个层次：
 *   - 队列层（q_push）：包数/字节双上限，超了丢最旧——只丢自己的队列，绝不反压 hub
 *     拖慢其它观众；
 *   - 帧层（adapt_drop）：按 TWCC 估计的发送码率判定拥塞，整帧丢弃非关键帧，丢后
 *     进入等关键帧状态，避免花屏。
 * 全部在会话 poller 线程执行，无需加锁。
 */
/**
 * @file play_session.c
 * @brief WebRTC 播放会话：STUN/DTLS 与 RTP 输入。
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

static int webrtc_h264_payload_is_key(const uint8_t *payload, size_t len);

static void webrtc_play_rtp_q_reset(zms_webrtc_session *s)
{
    size_t i;
    if (!s || !s->rt.q) {
        return;
    }
    for (i = 0; i < ZMS_WEBRTC_PLAY_RTPQ_CAP; ++i) {
        if (s->rt.q[i].buf) {
            zn_buf_unref(s->rt.q[i].buf);
        }
        s->rt.q[i].buf = NULL;
    }
    s->rt.q_r = 0;
    s->rt.q_w = 0;
    s->rt.q_n = 0;
    s->rt.q_bytes = 0;
}

int webrtc_play_rtp_q_ensure(zms_webrtc_session *s)
{
    if (!s) {
        return -1;
    }
    if (s->rt.q) {
        return 0;
    }
    s->rt.q =
        (zms_webrtc_play_rtp_slot *)calloc(ZMS_WEBRTC_PLAY_RTPQ_CAP, sizeof(*s->rt.q));
    return s->rt.q ? 0 : -1;
}

size_t webrtc_play_rtp_q_clear(zms_webrtc_session *s)
{
    size_t dropped_bytes = 0;
    unsigned dropped_pkts = 0;
    unsigned i;

    if (!s || !s->rt.q) {
        return 0;
    }
    for (i = 0; i < s->rt.q_n; ++i) {
        zms_webrtc_play_rtp_slot *slot =
            &s->rt.q[(s->rt.q_r + i) % ZMS_WEBRTC_PLAY_RTPQ_CAP];

        if (slot->buf) {
            dropped_bytes += zn_buf_len(slot->buf);
            zn_buf_unref(slot->buf);
            slot->buf = NULL;
            ++dropped_pkts;
        }
    }
    s->rt.q_dropped_bytes += dropped_bytes;
    s->rt.q_dropped_pkts += dropped_pkts;
    s->rt.q_r = s->rt.q_w;
    s->rt.q_n = 0;
    s->rt.q_bytes = 0;
    s->rt.adapt_in_frame = 0;
    s->rt.adapt_drop_frame = 0;
    s->rt.adapt_wait_key = 1;
    s->rt.adapt_dropping = 1;
    return dropped_bytes;
}

void webrtc_play_rtp_q_free(zms_webrtc_session *s)
{
    if (!s) {
        return;
    }
    webrtc_play_rtp_q_reset(s);
    free(s->rt.q);
    s->rt.q = NULL;
    s->rt.q_bytes_max = 0;
    s->rt.q_dropped_bytes = 0;
    s->rt.q_dropped_pkts = 0;
    s->rt.q_alloc_fail = 0;
    webrtc_play_nack_cache_free(s);
    s->rt.nack_hits = 0;
    s->rt.nack_misses = 0;
    s->rt.nack_retransmits = 0;
}

/*
 * 帧级自适应丢帧（仅 H264、仅在上层给了 send_bps 时启用）。
 *
 * 状态机：
 *   adapt_wait_key  丢帧后处于“等关键帧”态，此间非关键帧整帧丢弃；
 *   adapt_drop_frame 正在丢弃一个分片帧的剩余分片（直到 marker）；
 *   adapt_in_frame  当前是否在一个帧的中间（用于按帧而非按包计数）。
 *
 * 为什么丢整帧而非丢包：丢弃一帧里的部分 RTP 包会让该帧无法解码且污染后续参考，
 * 整帧丢弃 + 等下一个关键帧才能保证画面可恢复。
 *
 * 拥塞判据：用每秒滑动窗口估算实际发送码率 adapt_window_bps，接近或超过目标
 * send_bps 的 90%，或队内等待 EWMA > 150ms，即视为拥塞。丢一帧后保留 2 帧恢复
 * 窗口（adapt_hold_frames），避免连续丢帧造成长卡顿。
 *
 * 返回 1 表示该包被丢弃（调用方应跳过入队）。
 */
static int webrtc_play_adapt_drop(zms_webrtc_session *s, const uint8_t *rtp, size_t len)
{
    zms_rtp_packet pkt;
    int key = 0;
    int marker;
    uint64_t now;

    if (!s || s->rt.send_bps == 0 || s->video_codec != ZMS_CODEC_H264 || !rtp || len < 12) {
        return 0;
    }
    if (zms_rtp_parse(rtp, len, &pkt) != ZN_OK) {
        return 0;
    }
    key = webrtc_h264_payload_is_key(pkt.payload, pkt.payload_size);
    marker = pkt.hdr.marker;
    now = zn_monotonic_ms();
    if (s->rt.adapt_window_ms == 0 || now - s->rt.adapt_window_ms >= 1000) {
        if (s->rt.adapt_window_ms && now > s->rt.adapt_window_ms) {
            s->rt.adapt_window_bps =
                (uint32_t)(s->rt.adapt_window_bytes * 8000ull / (now - s->rt.adapt_window_ms));
        }
        s->rt.adapt_window_ms = now;
        s->rt.adapt_window_bytes = 0;
    }

    if (s->rt.adapt_wait_key) {
        if (!key) {
            if (!s->rt.adapt_in_frame) {
                ++s->rt.adapt_dropped_frames;
                s->rt.adapt_in_frame = 1;
            }
            s->rt.adapt_dropped_bytes += len;
            if (marker) {
                s->rt.adapt_in_frame = 0;
            }
            return 1;
        }
        s->rt.adapt_wait_key = 0;
        s->rt.adapt_dropping = 0;
        s->rt.adapt_in_frame = 0;
    }
    if (s->rt.adapt_drop_frame) {
        s->rt.adapt_dropped_bytes += len;
        if (marker) {
            s->rt.adapt_drop_frame = 0;
            s->rt.adapt_wait_key = 1;
            s->rt.adapt_dropping = 1;
            s->rt.adapt_need_key = 1;
        }
        return 1;
    }
    if (!s->rt.adapt_in_frame) {
        int congested =
            s->rt.adapt_window_bps > 0 &&
            ((uint64_t)s->rt.adapt_window_bps > (uint64_t)s->rt.send_bps * 9u / 10u ||
             s->rt.adapt_wait_ewma_ms > 150u);

        if (key) {
            s->rt.adapt_hold_frames = 0;
        }
        /* 帧起始：拥塞且不是关键帧则整帧丢弃；丢帧后保留 2 帧恢复窗口。 */
        if (!key && congested) {
            if (s->rt.adapt_hold_frames > 0) {
                --s->rt.adapt_hold_frames;
            } else {
                ++s->rt.adapt_dropped_frames;
                s->rt.adapt_dropped_bytes += len;
                s->rt.adapt_drop_frame = !marker;
                s->rt.adapt_hold_frames = 2;
                if (marker) {
                    s->rt.adapt_wait_key = 1;
                    s->rt.adapt_need_key = 1;
                }
                s->rt.adapt_dropping = 1;
                s->rt.adapt_in_frame = 0;
                return 1;
            }
        }
        s->rt.adapt_in_frame = 1;
    }
    s->rt.adapt_window_bytes += len;
    if (marker) {
        s->rt.adapt_in_frame = 0;
    }
    return 0;
}

/* 入队一个待发 RTP 包。视频先过帧级自适应（可能直接丢）。队列满（包数/字节任一
 * 超限）时丢最旧——保证队列长度有界、延迟不无限增长，且丢最旧对播放连续性冲击最小。
 * drop 日志按百包节流，避免丢包风暴刷屏。 */
int webrtc_play_rtp_q_push(zms_webrtc_session *s, zms_rtp_mux_track track,
                                  const uint8_t *rtp, size_t len)
{
    zms_webrtc_play_rtp_slot *slot;
    zn_buf *buf;
    void *dst;
    uint64_t drop_bytes = 0;
    uint64_t drop_pkts = 0;
    uint64_t drops_before;
    if (!s || !rtp || len < 12 || len > ZMS_WEBRTC_PLAY_RTP_SLOT_MAX || !s->poller) {
        return -1;
    }
    if (track == ZMS_RTP_MUX_TRACK_VIDEO && webrtc_play_adapt_drop(s, rtp, len)) {
        return -1;
    }
    if (webrtc_play_rtp_q_ensure(s) != 0) {
        ++s->rt.q_alloc_fail;
        return -1;
    }
    drops_before = s->rt.q_dropped_pkts;
    /* 包数/字节双上限：慢消费者只丢自己的队列，不反压 hub。 */
    while (s->rt.q_n > 0 &&
           (s->rt.q_n >= ZMS_WEBRTC_PLAY_RTPQ_CAP ||
            s->rt.q_bytes + len > ZMS_WEBRTC_PLAY_RTPQ_BYTES)) {
        zms_webrtc_play_rtp_slot *drop = &s->rt.q[s->rt.q_r % ZMS_WEBRTC_PLAY_RTPQ_CAP];
        size_t drop_len = drop->buf ? zn_buf_len(drop->buf) : 0;

        if (drop->buf) {
            zn_buf_unref(drop->buf);
            drop->buf = NULL;
        }
        if (s->rt.q_bytes >= drop_len) {
            s->rt.q_bytes -= drop_len;
        } else {
            s->rt.q_bytes = 0;
        }
        drop_bytes += drop_len;
        ++drop_pkts;
        s->rt.q_r = (s->rt.q_r + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
        --s->rt.q_n;
    }
    if (drop_pkts > 0) {
        s->rt.q_dropped_bytes += drop_bytes;
        s->rt.q_dropped_pkts += drop_pkts;
        if (drops_before == 0 || drops_before / 100u != s->rt.q_dropped_pkts / 100u) {
            zn_warn("[webrtc] rtpq_drop id=%s session=%u bytes=%llu pkts=%llu q=%u qbytes=%llu",
                     s->id, s->session_no, (unsigned long long)s->rt.q_dropped_bytes,
                     (unsigned long long)s->rt.q_dropped_pkts, s->rt.q_n,
                     (unsigned long long)s->rt.q_bytes);
        }
    }
    buf = zn_buf_alloc_local(s->poller, len);
    if (!buf) {
        ++s->rt.q_alloc_fail;
        return -1;
    }
    dst = (void *)zn_buf_data(buf);
    memcpy(dst, rtp, len);
    zn_buf_set_len(buf, len);
    slot = &s->rt.q[s->rt.q_w % ZMS_WEBRTC_PLAY_RTPQ_CAP];
    if (slot->buf) {
        size_t old_len = zn_buf_len(slot->buf);

        if (s->rt.q_bytes >= old_len) {
            s->rt.q_bytes -= old_len;
        } else {
            s->rt.q_bytes = 0;
        }
        zn_buf_unref(slot->buf);
    }
    slot->track = (uint8_t)track;
    slot->buf = buf;
    slot->enq_ms = zn_monotonic_ms();
    s->rt.q_w = (s->rt.q_w + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
    ++s->rt.q_n;
    s->rt.q_bytes += len;
    if (s->rt.q_bytes > s->rt.q_bytes_max) {
        s->rt.q_bytes_max = s->rt.q_bytes;
    }
    return 0;
}

void webrtc_play_nack_cache_free(zms_webrtc_session *s)
{
    unsigned i;

    if (!s || !s->rt.nack) {
        return;
    }
    for (i = 0; i < ZMS_WEBRTC_NACK_CACHE_PKTS; ++i) {
        if (s->rt.nack[i].buf) {
            zn_buf_unref(s->rt.nack[i].buf);
            s->rt.nack[i].buf = NULL;
        }
    }
    free(s->rt.nack);
    s->rt.nack = NULL;
    s->rt.nack_r = 0;
    s->rt.nack_w = 0;
    s->rt.nack_n = 0;
    s->rt.nack_bytes = 0;
}

static int webrtc_play_nack_cache_ensure(zms_webrtc_session *s)
{
    if (!s) {
        return -1;
    }
    if (s->rt.nack) {
        return 0;
    }
    s->rt.nack = (zms_webrtc_nack_slot *)calloc(ZMS_WEBRTC_NACK_CACHE_PKTS,
                                                   sizeof(*s->rt.nack));
    return s->rt.nack ? 0 : -1;
}

/* 把已发送（加密前的明文）视频包按 seq 存进 NACK 缓存环。同样受包数/字节双上限，
 * 超限丢最旧——缓存只为“最近丢失”重传服务，太老的包重传意义不大。存 seq 是为
 * 了 NACK 到来时能 O(n) 线性匹配（n 很小，不值得上哈希）。 */
static void webrtc_play_nack_cache_store(zms_webrtc_session *s, const uint8_t *rtp, size_t len)
{
    zms_webrtc_nack_slot *slot;
    zn_buf *buf;
    uint16_t seq;

    if (!s || !rtp || len < 12 || len > ZMS_WEBRTC_PLAY_RTP_SLOT_MAX || !s->poller ||
        webrtc_play_nack_cache_ensure(s) != 0) {
        return;
    }
    while (s->rt.nack_n > 0 &&
           (s->rt.nack_n >= ZMS_WEBRTC_NACK_CACHE_PKTS ||
            s->rt.nack_bytes + len > ZMS_WEBRTC_NACK_CACHE_BYTES)) {
        zms_webrtc_nack_slot *drop = &s->rt.nack[s->rt.nack_r % ZMS_WEBRTC_NACK_CACHE_PKTS];
        size_t drop_len = drop->buf ? zn_buf_len(drop->buf) : 0;

        if (drop->buf) {
            zn_buf_unref(drop->buf);
            drop->buf = NULL;
        }
        if (s->rt.nack_bytes >= drop_len) {
            s->rt.nack_bytes -= drop_len;
        } else {
            s->rt.nack_bytes = 0;
        }
        s->rt.nack_r = (s->rt.nack_r + 1) % ZMS_WEBRTC_NACK_CACHE_PKTS;
        --s->rt.nack_n;
    }
    buf = zn_buf_alloc_local(s->poller, len);
    if (!buf) {
        return;
    }
    memcpy((void *)zn_buf_data(buf), rtp, len);
    zn_buf_set_len(buf, len);
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    slot = &s->rt.nack[s->rt.nack_w % ZMS_WEBRTC_NACK_CACHE_PKTS];
    if (slot->buf) {
        size_t old_len = zn_buf_len(slot->buf);

        if (s->rt.nack_bytes >= old_len) {
            s->rt.nack_bytes -= old_len;
        } else {
            s->rt.nack_bytes = 0;
        }
        zn_buf_unref(slot->buf);
    }
    slot->seq = seq;
    slot->buf = buf;
    s->rt.nack_w = (s->rt.nack_w + 1) % ZMS_WEBRTC_NACK_CACHE_PKTS;
    ++s->rt.nack_n;
    s->rt.nack_bytes += len;
}

/* NACK 重传：按 seq 在缓存里找明文包，重发一次（不计入新缓存）。找不到就记 miss，
 * 客户端会因此在 RR/TWCC 反馈里体现丢包。重传走 send_rtp_item 但 cache_plain=0，
 * 避免把重传包再塞一遍缓存。 */
int webrtc_play_nack_retransmit(zms_webrtc_session *s, uint16_t seq)
{
    unsigned i;

    if (!s || !s->rt.nack || s->rt.nack_n == 0) {
        if (s) {
            ++s->rt.nack_misses;
        }
        return -1;
    }
    for (i = 0; i < s->rt.nack_n; ++i) {
        zms_webrtc_nack_slot *slot = &s->rt.nack[(s->rt.nack_r + i) % ZMS_WEBRTC_NACK_CACHE_PKTS];

        if (slot->seq != seq || !slot->buf) {
            continue;
        }
        ++s->rt.nack_hits;
        if (webrtc_play_send_rtp_item(s, (uint8_t)ZMS_RTP_MUX_TRACK_VIDEO,
                                      (const uint8_t *)zn_buf_data(slot->buf),
                                      zn_buf_len(slot->buf), 0) == 0) {
            ++s->rt.nack_retransmits;
            return 0;
        }
        return -1;
    }
    ++s->rt.nack_misses;
    return -1;
}

/*
 * 真正把明文 RTP 加密后发出。顺序：
 *   1) 拷进会话 io_buf（复用，避免每包分配）；
 *   2) PT 重映射到本会话协商的 payload type；
 *   3) 视频若协商了 TWCC 扩展，追加 transport-wide cc 扩展（供带宽估计）；
 *   4) 保护（加密）前把明文额外留一份 plain_copy，供 NACK 缓存与诊断回环校验；
 *   5) SRTP protect → udp 发送；
 *   6) 视频首次若干包打印一行发送诊断。
 * plain_copy 放在 io_buf 尾部预留的空间里，条件是包够小放得下；放不下就跳过 NACK
 * 缓存（不影响发送本身）。
 */
static int webrtc_play_send_rtp_item_impl(zms_webrtc_session *s, uint8_t track, const uint8_t *rtp,
                                          size_t len, int cache_plain)
{
    struct zms_webrtc_srtp *srtp;
    zms_webrtc_gateway_ingest_cfg gw;
    size_t out_len;
    size_t cap;
    size_t plain_len = len;
    int is_video = track == (uint8_t)ZMS_RTP_MUX_TRACK_VIDEO;
    uint8_t *plain_copy;
    if (!s || !rtp || len < 12 || !s->io_buf || s->io_cap < ZMS_WEBRTC_PLAY_CRYPT_BYTES ||
        len + ZMS_WEBRTC_SRTP_TAG_LEN > s->io_cap) {
        return -1;
    }
    cap = s->io_cap;
    srtp = is_video ? s->video_srtp : s->audio_srtp;
    memcpy(s->io_buf, rtp, len);
    webrtc_play_gateway_cfg(s, &gw);
    zms_webrtc_gateway_remap_pt_egress(s->io_buf, len, &gw, is_video);
    if (is_video && s->rt.twcc_ext_id > 0 &&
        len + 8u <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX) {
        size_t ext_len = 0;
        if (zms_webrtc_rtp_add_twcc_ext(s->io_buf, len, ZMS_WEBRTC_PLAY_RTP_SLOT_MAX,
                                        s->rt.twcc_ext_id, s->rt.twcc_seq++, &ext_len) > 0) {
            len = ext_len;
        }
    }
    out_len = len;
    if (!srtp) {
        webrtc_play_diag(s, "send_pre", "srtp null");
        return -1;
    }
    plain_copy = s->io_buf + ZMS_WEBRTC_PLAY_RTP_SLOT_MAX;
    if (len + ZMS_WEBRTC_SRTP_TAG_LEN <= cap && len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX &&
        plain_copy + len <= s->io_buf + cap) {
        memcpy(plain_copy, s->io_buf, len);
    }
    if (zms_webrtc_srtp_protect(srtp, s->io_buf, &out_len, cap) != 0) {
        static unsigned fail_log;
        if (fail_log < 8) {
            ++fail_log;
            zn_warn("[webrtc] srtp protect fail id=%s track=%u pt=%u len=%zu", s->id,
                     (unsigned)track, (unsigned)zms_rtp_payload_type(s->io_buf, len), len);
        }
        return -1;
    }
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
    if (is_video && len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX && len + ZMS_WEBRTC_SRTP_TAG_LEN <= cap) {
        webrtc_play_srtp_roundtrip_check(s, plain_copy, len, out_len);
    }
#endif
    if (zms_webrtc_session_send_udp(s, s->io_buf, out_len) != 0) {
        return -1;
    }
    if (is_video && cache_plain) {
        webrtc_play_nack_cache_store(s, rtp, plain_len);
    }
    if (is_video) {
        const uint8_t *plain = len <= ZMS_WEBRTC_PLAY_RTP_SLOT_MAX ? plain_copy : s->io_buf;
        if (s->play_out_logged < 8) {
            ++s->play_out_logged;
            zn_info("[webrtc] play srtp out id=%s pt=%u seq=%u ssrc=%u len=%zu->%zu peer=%s:%u",
                     s->id, (unsigned)zms_rtp_payload_type(plain, len),
                     (unsigned)(((uint16_t)plain[2] << 8) | plain[3]),
                     (unsigned)(((uint32_t)plain[8] << 24) | ((uint32_t)plain[9] << 16) |
                                ((uint32_t)plain[10] << 8) | plain[11]),
                     len, out_len, s->ice_host.peer_ip, (unsigned)s->ice_host.peer_port);
        }
    }
    return 0;
}

/* 发送包的外壳：Windows 上用 __try/__except 兜住访问违例（历史上的 0xc0000005
 * 崩溃，通常来自底层 SRTP/缓冲区越界），把硬崩降级为一次发送失败 + 告警，避免整个
 * 进程挂掉。其它平台直接调用。 */
int webrtc_play_send_rtp_item(zms_webrtc_session *s, uint8_t track, const uint8_t *rtp,
                                     size_t len, int cache_plain)
{
#if defined(_WIN32)
    __try {
        return webrtc_play_send_rtp_item_impl(s, track, rtp, len, cache_plain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        zn_warn("[webrtc] FAULT send id=%s code=0x%08x track=%u len=%zu", s ? s->id : "?",
                 (unsigned)GetExceptionCode(), (unsigned)track, len);
        return -1;
    }
#else
    return webrtc_play_send_rtp_item_impl(s, track, rtp, len, cache_plain);
#endif
}

/* 令牌桶节流。桶容量 = 码率 × BURST_MS（至少容纳一个包，保证超大帧也能发），
 * 按经过时间补充令牌。令牌不足返回 0，调用方保留队列下次再发——这就是“平滑 I 帧
 * 突发、但绝不丢包”的 pacer。send_bps==0 表示不限速（直接放行）。 */
static int webrtc_play_pace_allow(zms_webrtc_session *s, size_t bytes)
{
    uint64_t now_ms;
    uint64_t cap;
    uint64_t add;

    if (!s || s->rt.send_bps == 0) {
        return 1;
    }
    now_ms = zn_monotonic_ms();
    cap = (uint64_t)s->rt.send_bps / 8000u * ZMS_WEBRTC_PACER_BURST_MS;
    if (cap < bytes) {
        cap = bytes;
    }
    if (s->rt.pace_last_ms == 0) {
        s->rt.pace_last_ms = now_ms;
        s->rt.pace_tokens = cap;
    } else if (now_ms > s->rt.pace_last_ms) {
        add = (uint64_t)s->rt.send_bps * (now_ms - s->rt.pace_last_ms) / 8000u;
        s->rt.pace_tokens += add;
        s->rt.pace_last_ms = now_ms;
    }
    if (s->rt.pace_tokens > cap) {
        s->rt.pace_tokens = cap;
    }
    if (s->rt.pace_tokens < bytes) {
        ++s->rt.pace_waits;
        return 0;
    }
    s->rt.pace_tokens -= bytes;
    return 1;
}

/* 出队发送最多 max_send 个包。每个包先过 pacer：令牌不足就 break 保留队列；令牌
 * 足够才真正 send，并更新“包在队列里停留时长”的 EWMA（adapt_wait_ewma_ms，供丢帧
 * 判据用）。有发出则顺手尝试启动 RTCP（首个发送后触发 SR/RR 节奏）。 */
unsigned webrtc_play_rtp_q_flush(zms_webrtc_session *s, unsigned max_send)
{
    unsigned sent = 0;
    if (!s || !s->rt.q) {
        return 0;
    }
    while (max_send > 0 && s->rt.q_n > 0) {
        zms_webrtc_play_rtp_slot *slot = &s->rt.q[s->rt.q_r % ZMS_WEBRTC_PLAY_RTPQ_CAP];
        size_t slot_len = slot->buf ? zn_buf_len(slot->buf) : 0;

        if (slot->buf && !webrtc_play_pace_allow(s, slot_len)) {
            /* 令牌不足：保留队列，pump 下一次 tick 继续。 */
            break;
        }
        if (slot->buf) {
            uint64_t now_ms = zn_monotonic_ms();
            uint64_t wait_ms = slot->enq_ms && now_ms > slot->enq_ms ? now_ms - slot->enq_ms : 0;

            s->rt.adapt_wait_ewma_ms = (s->rt.adapt_wait_ewma_ms * 7u + wait_ms) / 8u;
            if (s->rt.q_bytes >= slot_len) {
                s->rt.q_bytes -= slot_len;
            } else {
                s->rt.q_bytes = 0;
            }
            if (webrtc_play_send_rtp_item(s, slot->track, (const uint8_t *)zn_buf_data(slot->buf),
                                          slot_len, 1) == 0) {
                ++sent;
            }
            zn_buf_unref(slot->buf);
            slot->buf = NULL;
        }
        s->rt.q_r = (s->rt.q_r + 1) % ZMS_WEBRTC_PLAY_RTPQ_CAP;
        --s->rt.q_n;
        --max_send;
    }
    if (sent > 0) {
        webrtc_play_try_rtcp_boot(s);
    }
    return sent;
}

/* 判断 H264 RTP 载荷是否关键帧（供帧级自适应决定能否起播/恢复）。覆盖三种打包：
 *   单 NAL（type 5=IDR）；STAP-A（type 24，聚合包里任一 NAL 为 5 或 7）；FU-A 起始
 *   分片（type 28 且 S 位=1 且原始类型 5）。识别 SPS(7) 也算关键，因为带参数的帧
 *   可独立起播。 */
static int webrtc_h264_payload_is_key(const uint8_t *payload, size_t len)
{
    int type;

    if (!payload || len < 1) {
        return 0;
    }
    type = payload[0] & 0x1f;
    if (type == 5) {
        return 1;
    }
    if (type == 24) {
        size_t off = 1;
        while (off + 2 <= len) {
            size_t nlen = ((size_t)payload[off] << 8) | payload[off + 1];
            int nt;
            off += 2;
            if (nlen == 0 || off + nlen > len) {
                break;
            }
            nt = payload[off] & 0x1f;
            if (nt == 5 || nt == 7) {
                return 1;
            }
            off += nlen;
        }
        return 0;
    }
    if (type == 28 && len >= 2) {
        int start = (payload[1] >> 7) & 1;
        int nt = payload[1] & 0x1f;
        return start && nt == 5;
    }
    return 0;
}
