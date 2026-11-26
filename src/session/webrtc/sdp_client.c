/**
 * @file sdp_client.c
 * @brief WebRTC 主动连接 SDP：生成 offer、应用 answer、offer probe。
 */
/**
 * @file sdp.c
 * @brief WebRTC SDP：offer/answer 与 fmtp。
 */
#include "session/webrtc/session_internal.h"
#include "protocol/webrtc/media_internal.h"
#include "protocol/webrtc/ice_internal.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/hub.h"
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/protocol/rtsp/sdp.h"
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

int zms_webrtc_session_build_client_offer(zms_webrtc_session *s, int recvonly, char *offer,
                                          size_t offer_cap, size_t *offer_len)
{
    const char *host;
    const char *fp;
    char profile[16];
    char sprop[1536];
    char fmtp_line[1792];
    char cands[1024];
    size_t cand_len;
    int n;
    int i;

    if (!s || !s->ice_host.ctx || !offer || offer_cap == 0) {
        return -1;
    }
    host = zms_webrtc_server_advertise_host(zms_webrtc_server_instance());
    fp = zms_webrtc_dtls_fingerprint();
    if (!host || !host[0] || !fp) {
        return -1;
    }
    if (!s->local_ufrag[0]) {
        webrtc_rand_credential(s->local_ufrag, sizeof(s->local_ufrag), 8);
    }
    if (!s->local_pwd[0]) {
        webrtc_rand_credential(s->local_pwd, sizeof(s->local_pwd), 22);
    }
    if (zms_webrtc_ice_prepare_client(s->ice_host.ctx, s->local_ufrag, s->local_pwd, host, s->port) != 0) {
        return -1;
    }
    /* 不在这里等 ICE gather：本函数跑在 poller 上，sleep 会卡住整个事件循环。
     * 候选由后续 RTCP/timer 侧补发（ice_gather_done 回调）。 */
    (void)i;

    profile[0] = '\0';
    sprop[0] = '\0';
    if (recvonly) {
        snprintf(profile, sizeof(profile), "42e01f");
    } else {
        webrtc_h264_load_fmtp(s, profile, sizeof(profile), sprop, sizeof(sprop));
    }
    if (sprop[0]) {
        snprintf(fmtp_line, sizeof(fmtp_line),
                 "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s;sprop-"
                 "parameter-sets=%s",
                 profile, sprop);
    } else {
        snprintf(fmtp_line, sizeof(fmtp_line),
                 "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%s", profile);
    }

    s->video_pt = 96;
    s->video_codec = ZMS_CODEC_H264;
    s->answer_has_video = 1;
    s->answer_has_audio = 0;
    snprintf(s->video_mid, sizeof(s->video_mid), "0");
    s->rt.twcc_ext_id = recvonly ? 0 : 3;

    n = snprintf(offer, offer_cap,
                 "v=0\r\n"
                 "o=- %llu %u IN IP4 %s\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "a=group:BUNDLE 0\r\n"
                 "a=msid-semantic: WMS *\r\n"
                 "m=video %u UDP/TLS/RTP/SAVPF 96\r\n"
                 "c=IN IP4 %s\r\n"
                 "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                 "a=ice-ufrag:%s\r\n"
                 "a=ice-pwd:%s\r\n"
                 "a=fingerprint:sha-256 %s\r\n"
                 "a=setup:active\r\n"
                 "a=mid:0\r\n"
                 "a=%s\r\n"
                 "a=rtcp-mux\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 %s\r\n"
                 "a=rtcp-fb:96 nack\r\n"
                 "a=rtcp-fb:96 nack pli\r\n",
                 (unsigned long long)time(NULL), (unsigned)s->session_no, host, (unsigned)s->port,
                 host, s->local_ufrag, s->local_pwd, fp, recvonly ? "recvonly" : "sendonly",
                 fmtp_line);
    if (n < 0 || (size_t)n >= offer_cap) {
        return -1;
    }
    if (!recvonly) {
        int r = snprintf(offer + n, offer_cap - (size_t)n,
                         "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-"
                         "extensions-01\r\n"
                         "a=rtcp-fb:96 transport-cc\r\n"
                         "a=ssrc:1 cname:zms\r\n"
                         "a=ssrc:1 msid:zms zmsv\r\n");
        if (r < 0 || (size_t)r >= offer_cap - (size_t)n) {
            return -1;
        }
        n += r;
    }
    {
        int r = snprintf(offer + n, offer_cap - (size_t)n,
                         "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n", host,
                         (unsigned)s->port);
        if (r < 0 || (size_t)r >= offer_cap - (size_t)n) {
            return -1;
        }
        n += r;
    }
    cand_len = zms_webrtc_ice_sdp_candidates(s->ice_host.ctx, cands, sizeof(cands));
    if (cand_len > 0) {
        if (cand_len >= offer_cap - (size_t)n) {
            return -1;
        }
        memcpy(offer + n, cands, cand_len);
        n += (int)cand_len;
    }
    {
        int r = snprintf(offer + n, offer_cap - (size_t)n, "a=end-of-candidates\r\n");
        if (r < 0 || (size_t)r >= offer_cap - (size_t)n) {
            return -1;
        }
        n += r;
    }
    if (offer_len) {
        *offer_len = (size_t)n;
    }
    zn_info("[webrtc] client offer id=%s %s host=%s:%u len=%d", s->id,
             recvonly ? "recvonly" : "sendonly", host, (unsigned)s->port, n);
    return 0;
}

typedef struct webrtc_rtpmap_ctx {
    int want_pt;
    char enc[16];
    int rate;
    char params[64];
    int found;
} webrtc_rtpmap_ctx;

static void webrtc_rtpmap_ctx_cb(void *param, const char *name, const char *value)
{
    webrtc_rtpmap_ctx *ctx = (webrtc_rtpmap_ctx *)param;
    int pt = 0;
    int rate = 0;

    if (!ctx || ctx->found || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    ctx->enc[0] = '\0';
    ctx->params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, ctx->enc, &rate, ctx->params) != 0 || pt != ctx->want_pt) {
        return;
    }
    ctx->rate = rate;
    ctx->found = 1;
}

static int webrtc_sdp_rtpmap_for_pt(sdp_t *sdp, int media_idx, int pt, char *enc, size_t enc_cap,
                                    int *rate, char *params, size_t params_cap)
{
    webrtc_rtpmap_ctx ctx;

    if (!sdp || !enc || !rate) {
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.want_pt = pt;
    (void)sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_rtpmap_ctx_cb, &ctx);
    if (!ctx.found) {
        return -1;
    }
    snprintf(enc, enc_cap, "%s", ctx.enc);
    if (params && params_cap) {
        snprintf(params, params_cap, "%s", ctx.params);
    }
    *rate = ctx.rate;
    return 0;
}

static int webrtc_client_answer_video_pt(sdp_t *sdp, int *out_pt)
{
    int media_idx;
    int fmts[16];
    int n;
    int i;

    if (!sdp || !out_pt) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "video");
    if (media_idx < 0) {
        return -1;
    }
    n = sdp_media_formats(sdp, media_idx, fmts, (int)(sizeof(fmts) / sizeof(fmts[0])));
    for (i = 0; i < n; ++i) {
        char enc[16];
        char params[64];
        int rate = 0;

        if (webrtc_sdp_rtpmap_for_pt(sdp, media_idx, fmts[i], enc, sizeof(enc), &rate, params,
                                     sizeof(params)) != 0) {
            continue;
        }
        if (webrtc_codec_from_encoding(enc) == ZMS_CODEC_H264) {
            *out_pt = fmts[i];
            return 0;
        }
    }
    return -1;
}

int zms_webrtc_session_probe_offer(const char *offer, size_t offer_len, char *ufrag,
                                   size_t ufrag_cap, int *video_pt)
{
    sdp_t *sdp;
    char pwd[128];

    if (!offer || offer_len == 0 || !ufrag || ufrag_cap == 0) {
        return -1;
    }
    ufrag[0] = '\0';
    if (video_pt) {
        *video_pt = -1;
    }
    sdp = sdp_parse(offer, (int)offer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, ufrag, ufrag_cap, pwd, sizeof(pwd)) != 0) {
        sdp_destroy(sdp);
        return -1;
    }
    if (video_pt) {
        int pt = -1;
        if (webrtc_client_answer_video_pt(sdp, &pt) == 0) {
            *video_pt = pt;
        }
    }
    sdp_destroy(sdp);
    return 0;
}

int zms_webrtc_session_apply_client_answer(zms_webrtc_session *s, const char *answer,
                                           size_t answer_len)
{
    sdp_t *sdp;
    int pt = 0;

    if (!s || !answer || answer_len == 0) {
        return -1;
    }
    sdp = sdp_parse(answer, (int)answer_len);
    if (!sdp) {
        return -1;
    }
    if (webrtc_parse_remote_ice(sdp, s->remote_ufrag, sizeof(s->remote_ufrag), s->remote_pwd,
                                sizeof(s->remote_pwd)) != 0) {
        zn_warn("[webrtc] client answer missing ICE creds id=%s", s->id);
        sdp_destroy(sdp);
        return -1;
    }
    if (webrtc_client_answer_video_pt(sdp, &pt) == 0) {
        s->video_pt = (uint8_t)pt;
        s->video_codec = ZMS_CODEC_H264;
        s->answer_has_video = 1;
        zn_info("[webrtc] client answer video pt=%d id=%s", pt, s->id);
    } else {
        zn_warn("[webrtc] client answer has no usable video id=%s", s->id);
        sdp_destroy(sdp);
        return -1;
    }
    s->answer_has_audio = 0;
    sdp_destroy(sdp);
    if (s->ice_host.ctx && zms_webrtc_ice_connect_client(s->ice_host.ctx, answer, answer_len, s->remote_ufrag,
                                                s->remote_pwd) != 0) {
        return -1;
    }
    zn_info("[webrtc] client answer applied id=%s pt=%u", s->id, (unsigned)s->video_pt);
    return 0;
}
