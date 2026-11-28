/**
 * @file sdp_answer.c
 * @brief WebRTC SDP 应答文本构造：video/audio/media 段、bundle、候选、TWCC。
 */
/**
 * @file sdp.c
 * @brief WebRTC SDP：offer/answer 与 fmtp。
 */
#include "session/webrtc/session_internal.h"
#include "protocol/webrtc/media_internal.h"
#include "protocol/webrtc/ice_internal.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream/hub.h"
#include "zcs/session/webrtc/webrtc_server.h"
#include "zcs/protocol/rtsp/sdp.h"
#include "sdp.h"
#include "sdp_internal.h"
#include "sdp-a-fmtp.h"
#include "sdp-a-rtpmap.h"
#include "zn/util/log.h"
#include "zn/platform.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define webrtc_strcasecmp _stricmp
#else
#include <strings.h>
#define webrtc_strcasecmp strcasecmp
#endif

/* DTLS 角色由 offer 的 a=setup 决定：对方要求 passive（即对方愿做 server）时
 * 我们回 active 主动发起握手；否则我们做 server。角色必须与 offer 互补，
 * 否则两端都等对方先发 ClientHello，DTLS 永远握不上手。 */
static const char *webrtc_dtls_setup_answer(const zcs_webrtc_session *s)
{
    return s && s->dtls_as_client ? "active" : "passive";
}

/* 拼 a=group:BUNDLE 行，按 offer 的 mid 顺序列出。若一个有效 mid 都没有（异常
 * offer），退回一个占位 "0"，保证这一行语法合法、不至于让整个 answer 无效。 */
static int webrtc_snprintf_bundle(char *buf, size_t cap, const zcs_webrtc_session *s)
{
    int n;
    int i;

    if (!buf || cap == 0 || !s) {
        return -1;
    }
    n = snprintf(buf, cap, "a=group:BUNDLE");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    for (i = 0; i < s->offer_media_count; ++i) {
        const char *mid = s->offer_media_mid[i];

        if (!mid[0]) {
            continue;
        }
        n += snprintf(buf + n, cap > (size_t)n ? cap - (size_t)n : 0, " %s", mid);
        if ((size_t)n >= cap) {
            return -1;
        }
    }
    if (n == (int)strlen("a=group:BUNDLE")) {
        n += snprintf(buf + n, cap > (size_t)n ? cap - (size_t)n : 0, " 0");
    }
    n += snprintf(buf + n, cap > (size_t)n ? cap - (size_t)n : 0, "\r\n");
    return n;
}

static int webrtc_append_rejected_audio_play(char *answer, size_t answer_cap, size_t n,
                                             const zcs_webrtc_session *s, const char *mid)
{
    if (!s || !mid || !mid[0] || !s->local_ufrag[0] || !s->local_pwd[0]) {
        return -1;
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=audio 0 UDP/TLS/RTP/SAVPF 0\r\n"
                             "c=IN IP4 0.0.0.0\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=mid:%s\r\n"
                             "a=inactive\r\n",
                             s->local_ufrag, s->local_pwd, mid);
}

static int webrtc_append_rejected_application(char *answer, size_t answer_cap, size_t n,
                                              const zcs_webrtc_session *s, const char *mid)
{
    if (!s || !mid || !mid[0] || !s->local_ufrag[0] || !s->local_pwd[0]) {
        return -1;
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                             "c=IN IP4 0.0.0.0\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=mid:%s\r\n"
                             "a=inactive\r\n",
                             s->local_ufrag, s->local_pwd, mid);
}

/*
 * 播放方向的视频段：发送（sendonly）方向。VP8/VP9 用标准 rtpmap 即可；H264 还需
 * fmtp 带上 profile-level-id 与 sprop-parameter-sets（SPS/PPS 的 base64），否则
 * 浏览器无法初始化解码器。所有分支统一带 nack/pli 反馈与 a=ice-lite，表示本端
 * 是轻量 ICE 实现（不主动发 connectivity check）。
 */
static int webrtc_append_video_play(char *answer, size_t answer_cap, size_t n,
                                    const zcs_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *v, const char *mid)
{
    char profile[16];
    char sprop[1536];
    char fmtp_line[1792];

    switch (v->codec) {
    case ZCS_CODEC_VP8:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:passive\r\n"
                                 "a=mid:%s\r\n"
                                 "a=sendonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP8/90000\r\n"
                                 "a=rtcp-fb:%u nack\r\n"
                                 "a=rtcp-fb:%u nack pli\r\n"
                                 "a=ssrc:1 cname:zcs\r\n"
                                 "a=ssrc:1 msid:zcs zcsv\r\n"
                                 "a=ssrc:1 mslabel:zcs\r\n"
                                 "a=ssrc:1 label:zcsv\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZCS_CODEC_VP9:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:passive\r\n"
                                 "a=mid:%s\r\n"
                                 "a=sendonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP9/90000\r\n"
                                 "a=rtcp-fb:%u nack\r\n"
                                 "a=rtcp-fb:%u nack pli\r\n"
                                 "a=ssrc:1 cname:zcs\r\n"
                                 "a=ssrc:1 msid:zcs zcsv\r\n"
                                 "a=ssrc:1 mslabel:zcs\r\n"
                                 "a=ssrc:1 label:zcsv\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZCS_CODEC_H264:
    default: {
        int r;

        webrtc_h264_load_fmtp(s, profile, sizeof(profile), sprop, sizeof(sprop));
        if (sprop[0]) {
            snprintf(fmtp_line, sizeof(fmtp_line),
                     "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s;sprop-"
                     "parameter-sets=%s",
                     profile, sprop);
        } else {
            snprintf(fmtp_line, sizeof(fmtp_line),
                     "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s", profile);
        }
        r = webrtc_sdp_append(answer, answer_cap, n,
                              "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                              "c=IN IP4 %s\r\n"
                              "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                              "a=ice-ufrag:%s\r\n"
                              "a=ice-pwd:%s\r\n"
                              "a=ice-lite\r\n"
                              "a=fingerprint:sha-256 %s\r\n"
                              "a=setup:passive\r\n"
                              "a=mid:%s\r\n"
                              "a=sendonly\r\n"
                              "a=rtcp-mux\r\n"
                              "a=rtpmap:%u H264/90000\r\n"
                              "a=fmtp:%u %s\r\n"
                              "a=rtcp-fb:%u nack\r\n"
                              "a=rtcp-fb:%u nack pli\r\n"
                              "a=ssrc:1 cname:zcs\r\n"
                              "a=ssrc:1 msid:zcs zcsv\r\n"
                              "a=ssrc:1 mslabel:zcs\r\n"
                              "a=ssrc:1 label:zcsv\r\n"
                              "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                              (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                              s->local_pwd, fp, mid, (unsigned)v->pt, (unsigned)v->pt, fmtp_line,
                              (unsigned)v->pt, (unsigned)v->pt, host, (unsigned)s->port);
        if (r < 0) {
            zn_warn("[webrtc] SDP video H264 truncated app=%s stream=%s", s->app, s->stream);
        }
        return r;
    }
    }
}

static int webrtc_append_video_publish(char *answer, size_t answer_cap, size_t n,
                                       const zcs_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *v, const char *mid)
{
    switch (v->codec) {
    case ZCS_CODEC_VP8:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP8/90000\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZCS_CODEC_VP9:
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u VP9/90000\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)v->pt, host, (unsigned)s->port);
    case ZCS_CODEC_H264:
    default:
        return (int)n +
               snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                        "m=video %u UDP/TLS/RTP/SAVPF %u\r\n"
                        "c=IN IP4 %s\r\n"
                        "a=ice-ufrag:%s\r\n"
                        "a=ice-pwd:%s\r\n"
                        "a=ice-lite\r\n"
                        "a=fingerprint:sha-256 %s\r\n"
                        "a=setup:%s\r\n"
                        "a=mid:%s\r\n"
                        "a=recvonly\r\n"
                        "a=rtcp-mux\r\n"
                        "a=rtpmap:%u H264/90000\r\n"
                        "a=fmtp:%u "
                        "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
                        "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                        (unsigned)s->port, (unsigned)v->pt, host, s->local_ufrag, s->local_pwd, fp,
                        webrtc_dtls_setup_answer(s), mid, (unsigned)v->pt, (unsigned)v->pt, host,
                        (unsigned)s->port);
    }
}

/*
 * 播放方向的 audio 段构造。这里同时承载两个独立职能：发送源音轨（send）与
 * 接收对讲音频（recv），二者组合出 sendrecv/sendonly/recvonly 方向。
 *
 * loopback 是专门的自测兼容：既没有对讲消费方、又没有源音轨时，若对讲 codec
 * 是 G711，就把它当成 sendrecv 回环——浏览器能自己听到自己发的音频，便于验证
 * 麦克风/编解码链路，而无需真的有对端。循环会污染资源，故只在 G711（无状态）
 * 且无真实消费方时启用。
 */
static int webrtc_append_audio_play(char *answer, size_t answer_cap, size_t n,
                                    const zcs_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *a, const char *asc_hex, const char *mid)
{
    char fmts[48];
    const char *dir;
    int send;
    int recv;
    int loopback;
    int r;
    size_t used = 0;

    (void)asc_hex;
    send = a && a->codec != ZCS_CODEC_INVALID && s->answer_has_audio;
    recv = s->talk_enabled;
    if (!send && !recv) {
        zn_warn("[webrtc] no play audio to answer app=%s stream=%s", s->app, s->stream);
        return -1;
    }
    /* 无对讲消费者且源流无音轨时，回环给同一 viewer 便于浏览器自测。 */
    loopback = recv && !send && !zcs_webrtc_server_has_talk_audio_cb() &&
               (s->talk_codec == ZCS_CODEC_G711U || s->talk_codec == ZCS_CODEC_G711A);
    fmts[0] = '\0';
    if (send) {
        used += (size_t)snprintf(fmts + used, sizeof(fmts) - used, " %u", (unsigned)a->pt);
    }
    if (recv && (!send || s->talk_pt != a->pt)) {
        used += (size_t)snprintf(fmts + used, sizeof(fmts) - used, " %u", (unsigned)s->talk_pt);
    }
    dir = ((send && recv) || loopback) ? "sendrecv" : (send ? "sendonly" : "recvonly");
    r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                 "m=audio %u UDP/TLS/RTP/SAVPF%s\r\n"
                 "c=IN IP4 %s\r\n"
                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                 "a=ice-ufrag:%s\r\n"
                 "a=ice-pwd:%s\r\n"
                 "a=ice-lite\r\n"
                 "a=fingerprint:sha-256 %s\r\n"
                 "a=setup:passive\r\n"
                 "a=mid:%s\r\n"
                 "a=%s\r\n"
                 "a=rtcp-mux\r\n",
                 (unsigned)s->port, fmts, host, s->local_ufrag, s->local_pwd, fp, mid, dir);
    if (r < 0 || (size_t)r >= answer_cap - n) {
        return -1;
    }
    n += (size_t)r;

    if (send) {
        if (a->codec == ZCS_CODEC_OPUS) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u opus/%d/2\r\n"
                         "a=fmtp:%u minptime=10;useinbandfec=1\r\n",
                         (unsigned)a->pt, a->rate > 0 ? a->rate : 48000, (unsigned)a->pt);
        } else if (a->codec == ZCS_CODEC_G711A) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u PCMA/8000/1\r\n", (unsigned)a->pt);
        } else if (a->codec == ZCS_CODEC_G711U) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u PCMU/8000/1\r\n", (unsigned)a->pt);
        } else {
            zn_warn("[webrtc] refuse play audio codec=%s", zcs_codec_name(a->codec));
            return -1;
        }
        if (r < 0 || (size_t)r >= answer_cap - n) {
            return -1;
        }
        n += (size_t)r;
    }
    if (send || loopback) {
        r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                     "a=ssrc:2 cname:zcs\r\n"
                     "a=ssrc:2 msid:zcs zcsa\r\n"
                     "a=ssrc:2 mslabel:zcs\r\n"
                     "a=ssrc:2 label:zcsa\r\n");
        if (r < 0 || (size_t)r >= answer_cap - n) {
            return -1;
        }
        n += (size_t)r;
    }
    if (recv && (!send || s->talk_pt != a->pt)) {
        if (s->talk_codec == ZCS_CODEC_G711U) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u PCMU/8000/1\r\n", (unsigned)s->talk_pt);
        } else if (s->talk_codec == ZCS_CODEC_G711A) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u PCMA/8000/1\r\n", (unsigned)s->talk_pt);
        } else if (s->talk_codec == ZCS_CODEC_OPUS) {
            r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                         "a=rtpmap:%u opus/%d/2\r\n", (unsigned)s->talk_pt,
                         s->talk_rate > 0 ? s->talk_rate : 48000);
        } else {
            r = 0;
        }
        if (r < 0 || (size_t)r >= answer_cap - n) {
            return -1;
        }
        n += (size_t)r;
    }
    r = snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n", host,
                 (unsigned)s->port);
    if (r < 0 || (size_t)r >= answer_cap - n) {
        return -1;
    }
    n += (size_t)r;
    return (int)n;
}

/* 推流方向（WHIP）的音频段：recvonly（我们收）。Opus 走标准 rtpmap+fmtp；
 * 其余（AAC）分支保留 mpeg4-generic 描述，供非浏览器推流端使用——浏览器 WHIP
 * 一般只推 Opus，但 RTP 推流网关可能用 AAC。 */
static int webrtc_append_audio_publish(char *answer, size_t answer_cap, size_t n,
                                       const zcs_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *a, const char *mid)
{
    if (a->codec == ZCS_CODEC_OPUS) {
        return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                                 "m=audio %u UDP/TLS/RTP/SAVPF %u\r\n"
                                 "c=IN IP4 %s\r\n"
                                 "a=ice-ufrag:%s\r\n"
                                 "a=ice-pwd:%s\r\n"
                                 "a=ice-lite\r\n"
                                 "a=fingerprint:sha-256 %s\r\n"
                                 "a=setup:%s\r\n"
                                 "a=mid:%s\r\n"
                                 "a=recvonly\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtpmap:%u opus/%d/2\r\n"
                                 "a=fmtp:%u minptime=10;useinbandfec=1\r\n"
                                 "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                                 (unsigned)s->port, (unsigned)a->pt, host, s->local_ufrag,
                                 s->local_pwd, fp, webrtc_dtls_setup_answer(s), mid,
                                 (unsigned)a->pt, a->rate > 0 ? a->rate : 48000, (unsigned)a->pt,
                                 host, (unsigned)s->port);
    }
    return (int)n + snprintf(answer + n, answer_cap > n ? answer_cap - n : 0,
                             "m=audio %u UDP/TLS/RTP/SAVPF %u\r\n"
                             "c=IN IP4 %s\r\n"
                             "a=ice-ufrag:%s\r\n"
                             "a=ice-pwd:%s\r\n"
                             "a=ice-lite\r\n"
                             "a=fingerprint:sha-256 %s\r\n"
                             "a=setup:%s\r\n"
                             "a=mid:%s\r\n"
                             "a=recvonly\r\n"
                             "a=rtcp-mux\r\n"
                             "a=rtpmap:%u mpeg4-generic/48000/2\r\n"
                             "a=fmtp:%u "
                             "streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;"
                             "indexlength=3;indexdeltalength=3\r\n"
                             "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
                             (unsigned)s->port, (unsigned)a->pt, host, s->local_ufrag, s->local_pwd,
                             fp, webrtc_dtls_setup_answer(s), mid, (unsigned)a->pt, (unsigned)a->pt,
                             host, (unsigned)s->port);
}

/*
 * 播放方向组装各媒体段。
 *
 * offer_media_count > 0 时严格按 offer 里 m-line 的原始顺序与 mid 逐个回复：
 * SDP 要求 answer 的 m-line 顺序和数量与 offer 一一对应，乱序会被浏览器拒绝。
 * 只有拿不到 offer 布局信息（count==0，老路径/测试）时，才退回用
 * offer_audio_before_video 这个粗粒度标志来还原大概顺序。
 */
static int webrtc_append_play_media(char *answer, size_t answer_cap, size_t n,
                                    const zcs_webrtc_session *s, const char *host, const char *fp,
                                    const webrtc_neg_track *v, const webrtc_neg_track *a,
                                    const char *asc_hex)
{
    int r;
    int i;

    if (s->offer_media_count > 0) {
        for (i = 0; i < s->offer_media_count; ++i) {
            const char *type = s->offer_media_type[i];
            const char *mid = s->offer_media_mid[i];

            if (strcmp(type, "video") == 0) {
                if (!s->answer_has_video) {
                    continue;
                }
                r = webrtc_append_video_play(answer, answer_cap, n, s, host, fp, v, mid);
            } else if (strcmp(type, "audio") == 0) {
                if (s->answer_has_audio || s->talk_enabled) {
                    r = webrtc_append_audio_play(answer, answer_cap, n, s, host, fp, a, asc_hex,
                                                 mid);
                } else {
                    r = webrtc_append_rejected_audio_play(answer, answer_cap, n, s, mid);
                }
            } else if (strcmp(type, "application") == 0) {
                r = webrtc_append_rejected_application(answer, answer_cap, n, s, mid);
            } else {
                continue;
            }
            if (r < 0 || (size_t)r >= answer_cap) {
                zn_warn("[webrtc] SDP play %s section failed app=%s stream=%s r=%d cap=%zu", type,
                         s->app, s->stream, r, answer_cap);
                return -1;
            }
            n = (size_t)r;
        }
        return (int)n;
    }

    if (s->offer_audio_before_video) {
        if (s->answer_has_audio || s->talk_enabled) {
            r = webrtc_append_audio_play(answer, answer_cap, n, s, host, fp, a, asc_hex,
                                         s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        } else if (s->offer_has_audio) {
            r = webrtc_append_rejected_audio_play(answer, answer_cap, n, s, s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
        if (s->answer_has_video) {
            r = webrtc_append_video_play(answer, answer_cap, n, s, host, fp, v, s->video_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
    } else {
        if (s->answer_has_video) {
            r = webrtc_append_video_play(answer, answer_cap, n, s, host, fp, v, s->video_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
        if (s->answer_has_audio || s->talk_enabled) {
            r = webrtc_append_audio_play(answer, answer_cap, n, s, host, fp, a, asc_hex,
                                         s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        } else if (s->offer_has_audio) {
            r = webrtc_append_rejected_audio_play(answer, answer_cap, n, s, s->audio_mid);
            if (r < 0 || (size_t)r >= answer_cap) {
                return -1;
            }
            n = (size_t)r;
        }
    }
    return (int)n;
}

/*
 * 推流方向的媒体段组装。WHIP 方向没有 offer_media_count 的完整布局（可能没存），
 * 只用 offer_audio_before_video 标志决定 m-line 先后——推流端一般固定先视频或先
 * 音频，粗粒度顺序已足够；关键是不能凭空多出 m-line，故只按 answer_has_* 输出。
 */
static int webrtc_append_publish_media(char *answer, size_t answer_cap, size_t n,
                                       const zcs_webrtc_session *s, const char *host,
                                       const char *fp, const webrtc_neg_track *v,
                                       const webrtc_neg_track *a)
{
    if (s->offer_audio_before_video) {
        if (s->answer_has_audio) {
            n = (size_t)webrtc_append_audio_publish(answer, answer_cap, n, s, host, fp, a,
                                                    s->audio_mid);
        }
        if (s->answer_has_video) {
            n = (size_t)webrtc_append_video_publish(answer, answer_cap, n, s, host, fp, v,
                                                    s->video_mid);
        }
    } else {
        if (s->answer_has_video) {
            n = (size_t)webrtc_append_video_publish(answer, answer_cap, n, s, host, fp, v,
                                                    s->video_mid);
        }
        if (s->answer_has_audio) {
            n = (size_t)webrtc_append_audio_publish(answer, answer_cap, n, s, host, fp, a,
                                                    s->audio_mid);
        }
    }
    return (int)n;
}

static int webrtc_insert_gathered_candidates(zcs_webrtc_session *s, char *answer, size_t cap)
{
    char extra[512];
    char *first;
    char *line_end;
    size_t extra_len;
    size_t old_len;
    size_t insert_at;
    int i;

    if (!s || !s->ice_host.ctx || !answer || cap == 0) {
        return 0;
    }
    /* 只插入"已经 gather 到"的候选，不等待：本函数在 poller 上，
     * 每个 answer 阻塞 300ms × 千路并发会把事件循环拖死。 */
    (void)i;
    extra_len = zcs_webrtc_ice_sdp_candidates(s->ice_host.ctx, extra, sizeof(extra));
    if (extra_len == 0) {
        return 0;
    }
    first = strstr(answer, "a=candidate:");
    if (!first) {
        return -1;
    }
    line_end = strstr(first, "\r\n");
    if (!line_end) {
        return -1;
    }
    old_len = strlen(answer);
    insert_at = (size_t)(line_end + 2 - answer);
    if (old_len + extra_len + 1 > cap) {
        return -1;
    }
    memmove(answer + insert_at + extra_len, answer + insert_at, old_len - insert_at + 1);
    memcpy(answer + insert_at, extra, extra_len);
    return 0;
}

/*
 * 若 offer 里带 transport-cc 扩展，在视频段补上 extmap 与 rtcp-fb 回显。
 * 必须复用 offer 里的 ext id（s->rt.twcc_ext_id），不能自选编号：extmap 的
 * 编号是两端协商的，answer 用不同 id 会让对端的 TWCC 反馈对不上。
 * 插入点选在 a=rtcp-mux 之后、同属视频段内，保证这些属性归属正确的 m-line。
 */
static int webrtc_answer_enable_twcc(const zcs_webrtc_session *s, char *answer, size_t cap)
{
    char extra[192];
    char *video;
    char *rtcp_mux;
    size_t insert_at;
    size_t extra_len;
    size_t old_len;

    if (!s || !answer || cap == 0 || s->rt.twcc_ext_id <= 0) {
        return 0;
    }
    video = strstr(answer, "\r\nm=video ");
    if (!video) {
        return 0;
    }
    rtcp_mux = strstr(video + 2, "\r\na=rtcp-mux\r\n");
    if (!rtcp_mux) {
        return 0;
    }
    extra_len = (size_t)snprintf(
        extra, sizeof(extra),
        "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=rtcp-fb:%u transport-cc\r\n",
        s->rt.twcc_ext_id, (unsigned)(s->video_pt ? s->video_pt : 96));
    if (extra_len == 0 || extra_len >= sizeof(extra)) {
        return -1;
    }
    old_len = strlen(answer);
    insert_at = (size_t)(rtcp_mux + strlen("\r\na=rtcp-mux\r\n") - answer);
    if (old_len + extra_len + 1 > cap) {
        return -1;
    }
    memmove(answer + insert_at + extra_len, answer + insert_at, old_len - insert_at + 1);
    memcpy(answer + insert_at, extra, extra_len);
    return 0;
}

/*
 * 构造 WHEP（播放）answer。步骤有依赖顺序，不可随意调换：
 *   1. 解析 offer 并抽取对端 ICE 凭据（后续 ICE setup 需要）；
 *   2. 生成自己的 ufrag/pwd，再协商 codec（会填 mut 里的 mid/pt/codec 等）；
 *   3. 用协商结果拼出 SDP 正文（bundle + 媒体段）；
 *   4. 追加 TWCC 扩展（依赖协商出的 video_pt 与 offer 的 ext id）；
 *   5. 最后才做 ICE setup 并把“已 gather 到的候选”插进正文。
 * 第 5 步必须在正文成型后：候选是往已写好的 a=candidate 行后插入的。
 */
int zcs_webrtc_session_build_answer(const zcs_webrtc_session *s, const char *offer,
                                    size_t offer_len, char *answer, size_t answer_cap,
                                    size_t *answer_len)
{
    sdp_t *sdp;
    int n;
    const char *host;
    const char *fp;
    char bundle_line[64];
    char asc_hex[128];
    webrtc_neg_track v, a;
    zcs_webrtc_session *mut = (zcs_webrtc_session *)s;

    if (!s || !offer || offer_len == 0 || !answer || answer_cap == 0) {
        return -1;
    }
    host = zcs_webrtc_server_advertise_host(zcs_webrtc_server_instance());
    fp = zcs_webrtc_dtls_fingerprint();
    sdp = sdp_parse(offer, (int)offer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, mut->remote_ufrag, sizeof(mut->remote_ufrag), mut->remote_pwd,
                                sizeof(mut->remote_pwd)) != 0) {
        sdp_destroy(sdp);
        return -1;
    }
    webrtc_rand_credential(mut->local_ufrag, sizeof(mut->local_ufrag), 8);
    webrtc_rand_credential(mut->local_pwd, sizeof(mut->local_pwd), 22);
    if (webrtc_session_negotiate(mut, sdp, 1) != 0) {
        zn_warn("[webrtc] WHEP codec negotiate failed app=%s stream=%s", s->app, s->stream);
        sdp_destroy(sdp);
        return -1;
    }
    sdp_destroy(sdp);
    asc_hex[0] = '\0';
    /* play 仅 Opus；AAC 已在 negotiate 阶段过滤，无需再取 ASC */

    v.codec = mut->video_codec;
    v.pt = mut->video_pt;
    a.codec = mut->audio_codec;
    a.pt = mut->audio_pt;
    a.rate = mut->audio_rate;
    a.channels = mut->audio_channels;

    webrtc_snprintf_bundle(bundle_line, sizeof(bundle_line), mut);
    n = snprintf(answer, answer_cap,
                 "v=0\r\n"
                 "o=- %llu 2 IN IP4 %s\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "%s"
                 "a=msid-semantic: WMS *\r\n",
                 (unsigned long long)time(NULL), host, bundle_line);
    n = webrtc_append_play_media(answer, answer_cap, (size_t)n, s, host, fp, &v, &a, asc_hex);
    if (n <= 0 || (size_t)n >= answer_cap) {
        zn_warn("[webrtc] answer too large app=%s stream=%s len=%d cap=%zu", s->app, s->stream, n,
                 answer_cap);
        return -1;
    }
    if (webrtc_answer_enable_twcc(s, answer, answer_cap) != 0) {
        zn_warn("[webrtc] TWCC answer insert failed app=%s stream=%s", s->app, s->stream);
    }
    n = (int)strlen(answer);
    zn_info("[webrtc] WHEP SDP body ready len=%d video=%d audio=%d twcc=%d app=%s stream=%s", n,
             mut->answer_has_video, mut->answer_has_audio, mut->rt.twcc_ext_id, s->app, s->stream);
    if (mut->ice_host.ctx) {
        if (!mut->remote_ufrag[0] || !mut->remote_pwd[0] || !mut->local_ufrag[0] ||
            !mut->local_pwd[0] || !host || !host[0]) {
            zn_warn("[webrtc] ICE setup skipped: missing auth/host app=%s stream=%s", s->app,
                     s->stream);
            return -1;
        }
        if (zcs_webrtc_ice_setup(mut->ice_host.ctx, offer, offer_len, mut->local_ufrag, mut->local_pwd,
                                 mut->remote_ufrag, mut->remote_pwd, host, s->port) != 0) {
            zn_warn("[webrtc] ICE setup failed app=%s stream=%s", s->app, s->stream);
            return -1;
        }
        if (webrtc_insert_gathered_candidates(mut, answer, answer_cap) != 0) {
            zn_warn("[webrtc] insert gathered candidates failed app=%s stream=%s", s->app,
                     s->stream);
        } else {
            n = (int)strlen(answer);
        }
    }
    if (answer_len) {
        *answer_len = (size_t)n;
    }
    zn_info("[webrtc] WHEP answer video=%s audio=%s app=%s stream=%s",
             zcs_codec_name(mut->video_codec),
             mut->answer_has_audio ? zcs_codec_name(mut->audio_codec) : "none", s->app, s->stream);
    return 0;
}

int zcs_webrtc_session_build_publish_answer(const zcs_webrtc_session *s, const char *offer,
                                            size_t offer_len, char *answer, size_t answer_cap,
                                            size_t *answer_len)
{
    sdp_t *sdp;
    int n;
    const char *host;
    const char *fp;
    char bundle_line[64];
    webrtc_neg_track v, a;
    zcs_webrtc_session *mut = (zcs_webrtc_session *)s;

    if (!s || !offer || offer_len == 0 || !answer || answer_cap == 0) {
        return -1;
    }
    host = zcs_webrtc_server_advertise_host(zcs_webrtc_server_instance());
    fp = zcs_webrtc_dtls_fingerprint();
    sdp = sdp_parse(offer, (int)offer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, mut->remote_ufrag, sizeof(mut->remote_ufrag), mut->remote_pwd,
                                sizeof(mut->remote_pwd)) != 0) {
        sdp_destroy(sdp);
        return -1;
    }
    webrtc_rand_credential(mut->local_ufrag, sizeof(mut->local_ufrag), 8);
    webrtc_rand_credential(mut->local_pwd, sizeof(mut->local_pwd), 22);
    if (webrtc_session_negotiate(mut, sdp, 0) != 0) {
        zn_warn("[webrtc] WHIP codec negotiate failed app=%s stream=%s", s->app, s->stream);
        sdp_destroy(sdp);
        return -1;
    }
    mut->dtls_as_client = webrtc_offer_setup_passive(sdp) ? 1 : 0;
    /* 把 offer 里的 H264 sprop（SPS/PPS）存下来：推流端可能不在首帧带参数集，
     * 提前从 offer 拿到能保证解码端尽早有参数可用。失败无害，静默忽略。 */
    (void)webrtc_publish_store_offer_h264_sprop(mut, sdp);
    sdp_destroy(sdp);

    v.codec = mut->video_codec;
    v.pt = mut->video_pt;
    a.codec = mut->audio_codec;
    a.pt = mut->audio_pt;
    a.rate = mut->audio_rate;
    a.channels = mut->audio_channels;

    webrtc_snprintf_bundle(bundle_line, sizeof(bundle_line), mut);
    n = snprintf(answer, answer_cap,
                 "v=0\r\n"
                 "o=- %llu 2 IN IP4 %s\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "%s"
                 "a=msid-semantic: WMS *\r\n",
                 (unsigned long long)time(NULL), host, bundle_line);
    n = webrtc_append_publish_media(answer, answer_cap, (size_t)n, s, host, fp, &v, &a);
    if (n <= 0 || (size_t)n >= answer_cap) {
        return -1;
    }
    if (mut->ice_host.ctx &&
        zcs_webrtc_ice_setup(mut->ice_host.ctx, offer, offer_len, mut->local_ufrag, mut->local_pwd,
                             mut->remote_ufrag, mut->remote_pwd, host, s->port) != 0) {
        return -1;
    }
    if (mut->ice_host.ctx && webrtc_insert_gathered_candidates(mut, answer, answer_cap) == 0) {
        n = (int)strlen(answer);
    }
    if (answer_len) {
        *answer_len = (size_t)n;
    }
    zn_info("[webrtc] WHIP answer video=%s audio=%s dtls=%s app=%s stream=%s",
             zcs_codec_name(mut->video_codec), zcs_codec_name(mut->audio_codec),
             mut->dtls_as_client ? "client" : "server", s->app, s->stream);
    return 0;
}
