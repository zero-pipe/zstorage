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
#include "zms/util/base64.h"
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define webrtc_strcasecmp _stricmp
#else
#include <strings.h>
#define webrtc_strcasecmp strcasecmp
#endif

/** SDP fmtp 只关心长度：失败返回已处理部分。 */
static size_t encode_b64(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t n = 0;
    (void)zms_base64_encode(in, in_len, out, cap, &n);
    return n;
}

static size_t decode_b64(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    (void)zms_base64_decode(in, out, cap, &n);
    return n;
}

int webrtc_sdp_append(char *answer, size_t answer_cap, size_t n, const char *fmt, ...)
{
    va_list ap;
    int w;
    size_t room;

    if (!answer || !fmt || n >= answer_cap) {
        return -1;
    }
    room = answer_cap - n;
    va_start(ap, fmt);
    w = vsnprintf(answer + n, room, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= room) {
        return -1;
    }
    return (int)(n + (size_t)w);
}

/* 生成 ICE 的 ufrag/pwd 随机串。用 rand() 而非 CSPRNG：这里只要“不易碰撞、
 * 不可预测性足够防误连”即可，不承担密码学安全职责；真正的媒体加密由 DTLS
 * 协商的 SRTP 密钥保证。因此不引入平台相关的强随机源。 */
void webrtc_rand_credential(char *out, size_t cap, size_t want)
{
    static const char k_alph[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t i;

    if (!out || cap == 0 || want == 0) {
        return;
    }
    if (want >= cap) {
        want = cap - 1;
    }
    for (i = 0; i < want; ++i) {
        out[i] = k_alph[(unsigned)rand() % (sizeof(k_alph) - 1)];
    }
    out[want] = '\0';
}

int webrtc_parse_remote_ice(sdp_t *sdp, char *ufrag, size_t ufrag_cap, char *pwd,
                                   size_t pwd_cap)
{
    const char *v;
    int i, n;

    if (!sdp) {
        return -1;
    }
    v = sdp_attribute_find(sdp, "ice-ufrag");
    if (!v || !v[0]) {
        n = sdp_media_count(sdp);
        for (i = 0; i < n; ++i) {
            v = sdp_media_attribute_find(sdp, i, "ice-ufrag");
            if (v && v[0]) {
                break;
            }
        }
    }
    if (!v || !v[0]) {
        return -1;
    }
    strncpy(ufrag, v, ufrag_cap - 1);
    ufrag[ufrag_cap - 1] = '\0';

    v = sdp_attribute_find(sdp, "ice-pwd");
    if (!v || !v[0]) {
        n = sdp_media_count(sdp);
        for (i = 0; i < n; ++i) {
            v = sdp_media_attribute_find(sdp, i, "ice-pwd");
            if (v && v[0]) {
                break;
            }
        }
    }
    if (!v || !v[0]) {
        return -1;
    }
    strncpy(pwd, v, pwd_cap - 1);
    pwd[pwd_cap - 1] = '\0';
    return 0;
}

int webrtc_media_index(sdp_t *sdp, const char *media_type)
{
    int i, n;

    if (!sdp || !media_type) {
        return -1;
    }
    n = sdp_media_count(sdp);
    for (i = 0; i < n; ++i) {
        if (sdp_media_type(sdp, i) && strcmp(sdp_media_type(sdp, i), media_type) == 0) {
            return i;
        }
    }
    return -1;
}

static int webrtc_offer_has_media(sdp_t *sdp, const char *media_type)
{
    return webrtc_media_index(sdp, media_type) >= 0;
}

/*
 * 从 SDP encoding 名推断 codec。两处刻意为之：
 *   - 先判 h264/h265/av1 等真编码，最后才排除 rtx/red/ulpfec/flexfec：
 *     这些是重传/冗余/FEC 的辅助编码，不能当可协商的媒体编码，必须显式丢弃，
 *     否则会误把 RTX 的 pt 当主编码。
 *   - 用 strstr 子串匹配而非精确相等，因为实时通信里 encoding 名可能有前后缀
 *     （如 "H264" 之外还有带 profile 的写法），子串更宽容。
 */
zms_codec_id webrtc_codec_from_encoding(const char *encoding)
{
    char lower[32];
    size_t i;

    if (!encoding || !encoding[0]) {
        return ZMS_CODEC_INVALID;
    }
    for (i = 0; encoding[i] && i + 1 < sizeof(lower); ++i) {
        lower[i] = (char)tolower((unsigned char)encoding[i]);
    }
    lower[i] = '\0';
    if (strstr(lower, "h264") || strstr(lower, "avc")) {
        return ZMS_CODEC_H264;
    }
    if (strstr(lower, "h265") || strstr(lower, "hevc")) {
        return ZMS_CODEC_H265;
    }
    if (strstr(lower, "av1") || strstr(lower, "av01")) {
        return ZMS_CODEC_AV1;
    }
    if (strstr(lower, "vp8")) {
        return ZMS_CODEC_VP8;
    }
    if (strstr(lower, "vp9")) {
        return ZMS_CODEC_VP9;
    }
    if (strstr(lower, "mpeg4-generic") || strstr(lower, "aac") || strstr(lower, "mp4a")) {
        return ZMS_CODEC_AAC;
    }
    if (strstr(lower, "opus")) {
        return ZMS_CODEC_OPUS;
    }
    if (strstr(lower, "pcmu") || strstr(lower, "g711u")) {
        return ZMS_CODEC_G711U;
    }
    if (strstr(lower, "pcma") || strstr(lower, "g711a")) {
        return ZMS_CODEC_G711A;
    }
    if (strstr(lower, "rtx") || strstr(lower, "red") || strstr(lower, "ulpfec") ||
        strstr(lower, "flexfec")) {
        return ZMS_CODEC_INVALID;
    }
    return ZMS_CODEC_INVALID;
}

/*
 * 某 codec 是否可与本端协商。约束按方向区分：
 *   - 视频：H264/VP8/VP9 通用可协商；H265/AV1 只在播放方向（for_play）协商，
 *     因为浏览器做推流（WHIP）时基本不吃这两种，协商了也没用。
 *   - 音频：浏览器音轨只认 Opus（主）与 G711（直通）；AAC 浏览器不解，明确拒绝。
 */
static int webrtc_codec_negotiable(zms_codec_id codec, int is_video, int for_play)
{
    if (is_video) {
        if (codec == ZMS_CODEC_H264 || codec == ZMS_CODEC_VP8 || codec == ZMS_CODEC_VP9) {
            return 1;
        }
        if (for_play && (codec == ZMS_CODEC_H265 || codec == ZMS_CODEC_AV1)) {
            return 1;
        }
        return 0;
    }
    /* 浏览器 WebRTC 音轨：Opus 为主，G711 直通；AAC 不可协商 */
    (void)for_play;
    return codec == ZMS_CODEC_OPUS || codec == ZMS_CODEC_G711A || codec == ZMS_CODEC_G711U;
}

static void webrtc_on_media_rtpmap_log(void *param, const char *name, const char *value)
{
    const char *tag = (const char *)param;
    int pt = 0;
    int rate = 0;
    char enc[16];
    char params[64];

    if (!name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) == 0) {
        zn_debug("[webrtc] sdp %s rtpmap pt=%d enc=%s rate=%d", tag ? tag : "?", pt, enc, rate);
    } else {
        zn_debug("[webrtc] sdp %s rtpmap parse fail value=[%s]", tag ? tag : "?", value);
    }
}

static void webrtc_log_media_rtpmaps(sdp_t *sdp, int media_idx, const char *tag)
{
    if (!sdp || media_idx < 0 || !tag) {
        return;
    }
    zn_debug("[webrtc] sdp %s media=%s idx=%d", tag, sdp_media_type(sdp, media_idx), media_idx);
    sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_on_media_rtpmap_log, (void *)tag);
}

typedef struct {
    int for_play;
    int is_video;
    zms_codec_id prefer;
    int want_pt;
    webrtc_neg_track *out;
    int done;
} webrtc_neg_pick_ctx;

static void webrtc_pick_rtpmap_cb(void *param, const char *name, const char *value)
{
    webrtc_neg_pick_ctx *pick = (webrtc_neg_pick_ctx *)param;
    webrtc_neg_track cand;
    int pt = 0;
    int rate = 0;
    char enc[16];
    char params[64];

    if (!pick || pick->done || !pick->out || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) != 0) {
        return;
    }
    if (pick->want_pt >= 0 && pt != pick->want_pt) {
        return;
    }
    cand.codec = webrtc_codec_from_encoding(enc);
    if (cand.codec == ZMS_CODEC_INVALID) {
        return;
    }
    if (!webrtc_codec_negotiable(cand.codec, pick->is_video, pick->for_play)) {
        return;
    }
    if (pick->for_play && pick->prefer != ZMS_CODEC_INVALID && cand.codec != pick->prefer) {
        return;
    }
    cand.pt = (uint8_t)pt;
    cand.rate = rate > 0 ? rate : (cand.codec == ZMS_CODEC_OPUS ? 48000 : 90000);
    cand.channels = 2;
    if (params[0]) {
        int ch = atoi(params);
        if (ch > 0) {
            cand.channels = ch;
        }
    }
    *pick->out = cand;
    pick->done = 1;
}

static void webrtc_h264_profile_lower(char *profile)
{
    char *p;

    if (!profile || !profile[0]) {
        return;
    }
    for (p = profile; *p; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }
}

static void webrtc_h264_profile_from_sps(const uint8_t *sps, size_t sps_len, char *profile,
                                         size_t cap)
{
    if (!profile || cap == 0) {
        return;
    }
    profile[0] = '\0';
    if (!sps || sps_len < 4) {
        return;
    }
    snprintf(profile, cap, "%02x%02x%02x", sps[1], sps[2], sps[3]);
    webrtc_h264_profile_lower(profile);
}

typedef struct {
    int want_pt;
    const char *found;
} webrtc_fmtp_pick_ctx;

static void webrtc_pick_fmtp_cb(void *param, const char *name, const char *value)
{
    webrtc_fmtp_pick_ctx *ctx = (webrtc_fmtp_pick_ctx *)param;
    int fmt;

    if (!ctx || !name || !value || strcmp(name, "fmtp") != 0) {
        return;
    }
    fmt = atoi(value);
    if (fmt == ctx->want_pt) {
        ctx->found = value;
    }
}

static const char *webrtc_sdp_fmtp_for_pt(sdp_t *sdp, int media_idx, int pt)
{
    webrtc_fmtp_pick_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.want_pt = pt;
    sdp_media_attribute_list(sdp, media_idx, "fmtp", webrtc_pick_fmtp_cb, &ctx);
    return ctx.found;
}

/*
 * H264 候选打分（多个 pt/编码描述里挑最合适的一个）。
 * 权重设计：
 *   - packetization-mode != 1 直接扣 8：mode 0 是不分片的单 NAL 模式，与实时
 *     传输所需的 FU-A 分片不兼容，基本不可用；
 *   - profile-level-id 完全一致 +32（最优先），前两字节（profile_idc+约束）一致
 *     +16（常可兼容），否则 +1（只是“能凑合”）。
 * 用累加分而非布尔过滤，保证即使没有完美匹配也能选出一个相对最优者。
 */
static int webrtc_h264_profile_score(const char *offer_plid, const char *want_plid, int pkt_mode)
{
    int score = 0;
    char offer[16];
    char want[16];

    if (pkt_mode != 1) {
        score -= 8;
    }
    if (!offer_plid || !offer_plid[0]) {
        return score;
    }
    snprintf(offer, sizeof(offer), "%s", offer_plid);
    webrtc_h264_profile_lower(offer);
    if (!want_plid || !want_plid[0]) {
        return score + 1;
    }
    snprintf(want, sizeof(want), "%s", want_plid);
    webrtc_h264_profile_lower(want);
    if (strcmp(offer, want) == 0) {
        return score + 32;
    }
    if (offer[0] == want[0] && offer[1] == want[1]) {
        return score + 16;
    }
    return score + 1;
}

typedef struct {
    sdp_t *sdp;
    int media_idx;
    const zms_webrtc_session *sess;
    char want_plid[16];
    webrtc_neg_track best;
    int best_score;
    int have_best;
} webrtc_h264_neg_ctx;

static void webrtc_negotiate_h264_cb(void *param, const char *name, const char *value)
{
    webrtc_h264_neg_ctx *ctx = (webrtc_h264_neg_ctx *)param;
    webrtc_neg_track cand;
    struct sdp_a_fmtp_h264_t h264;
    int pt = 0, fmt = 0, rate = 0, score;
    char enc[16];
    char params[64];

    if (!ctx || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) != 0) {
        return;
    }
    if (webrtc_codec_from_encoding(enc) != ZMS_CODEC_H264) {
        return;
    }
    memset(&h264, 0, sizeof(h264));
    {
        const char *fmtp = webrtc_sdp_fmtp_for_pt(ctx->sdp, ctx->media_idx, pt);
        char line[512];

        if (fmtp) {
            snprintf(line, sizeof(line), "%d %s", pt, fmtp);
            if (sdp_a_fmtp_h264(line, &fmt, &h264) != 0) {
                memset(&h264, 0, sizeof(h264));
            }
        }
    }
    score = webrtc_h264_profile_score(
        (h264.flags & SDP_A_FMTP_H264_PROFILE_LEVEL_ID) ? h264.profile_level_id : NULL,
        ctx->want_plid[0] ? ctx->want_plid : NULL,
        (h264.flags & SDP_A_FMTP_H264_PACKETIZATION_MODE) ? h264.packetization_mode : 1);
    if (ctx->have_best && score <= ctx->best_score) {
        return;
    }
    cand.codec = ZMS_CODEC_H264;
    cand.pt = (uint8_t)pt;
    cand.rate = 90000;
    cand.channels = 1;
    ctx->best = cand;
    ctx->best_score = score;
    ctx->have_best = 1;
}

static int webrtc_negotiate_h264_track(sdp_t *sdp, const zms_webrtc_session *sess,
                                       webrtc_neg_track *out)
{
    webrtc_h264_neg_ctx ctx;
    int media_idx;

    if (!sdp || !out) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "video");
    if (media_idx < 0) {
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.sdp = sdp;
    ctx.media_idx = media_idx;
    ctx.sess = sess;
    ctx.best_score = -999;
    if (sess && sess->source && sess->source->video.ready &&
        sess->source->video.profile_level_id[0]) {
        snprintf(ctx.want_plid, sizeof(ctx.want_plid), "%s", sess->source->video.profile_level_id);
    }
    sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_negotiate_h264_cb, &ctx);
    if (!ctx.have_best) {
        return -1;
    }
    *out = ctx.best;
    zn_info("[webrtc] h264 pt=%u profile=%s score=%d", (unsigned)out->pt,
             ctx.want_plid[0] ? ctx.want_plid : "?", ctx.best_score);
    return 0;
}

/*
 * 从媒体源里找 H264 的 SPS/PPS，供生成 answer 的 fmtp 用。三级来源：
 *   1. 源的 video config（最直接，已解析的 AVCDecoderConfigurationRecord）；
 *   2. gop_queue 里存的 video config（config 尚未落地但队列里有）；
 *   3. 临时 attach 一个从头读的 gop reader，扫前几个视频帧直到关键帧，
 *      从 ES 里抠 SPS/PPS。
 *
 * 第 3 步的 guard 很重要：WHEP 播放可能已经持有该队列的 gop reader，此处若
 * 再 attach_beginning 会多挂一个读者、读到相同数据并影响共享引用的生命周期，
 * 故检测到已有 reader 就直接放弃第 3 步。
 */
static int webrtc_h264_extract_ps(const zms_webrtc_session *s, const uint8_t **sps, size_t *sps_len,
                                  const uint8_t **pps, size_t *pps_len)
{
    size_t vcfg_len = 0;
    const uint8_t *vcfg;

    if (!s || !sps || !sps_len || !pps || !pps_len) {
        return 0;
    }
    *sps = *pps = NULL;
    *sps_len = *pps_len = 0;
    if (s->source) {
        vcfg = zms_media_source_video_config(s->source, &vcfg_len);
        if (vcfg && vcfg_len &&
            zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, sps, sps_len, pps, pps_len)) {
            return 1;
        }
        if (s->source->gop_queue) {
            vcfg = zms_gop_queue_video_config(s->source->gop_queue, &vcfg_len);
            if (vcfg && vcfg_len &&
                zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, sps, sps_len, pps, pps_len)) {
                return 1;
            }
        }
        /* WHEP 播放已持有该队列 gop reader；此处勿 attach_beginning。 */
        if (s->play.readers.gop) {
            return 0;
        }
        if (s->source->gop_queue) {
            zms_gop_reader *rd = zms_gop_reader_attach_beginning(s->source->gop_queue);
            zms_gop_slot slot;

            if (rd) {
                while (zms_gop_reader_read(rd, &slot) > 0) {
                    if (slot.track != ZMS_TRACK_VIDEO || !slot.data || slot.len == 0) {
                        continue;
                    }
                    if (zms_h264_es_extract_sps_pps(slot.data, slot.len, sps, sps_len, pps,
                                                    pps_len)) {
                        zms_gop_reader_detach(rd);
                        return 1;
                    }
                    if (slot.keyframe) {
                        break;
                    }
                }
                zms_gop_reader_detach(rd);
            }
        }
    }
    return 0;
}

void webrtc_h264_load_fmtp(const zms_webrtc_session *s, char *profile, size_t profile_cap,
                                  char *sprop, size_t sprop_cap)
{
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    char sps_b64[512], pps_b64[256];
    size_t vcfg_len = 0;
    const uint8_t *vcfg;

    if (profile && profile_cap) {
        profile[0] = '\0';
    }
    if (sprop && sprop_cap) {
        sprop[0] = '\0';
    }
    if (s && s->source && s->source->video.ready && s->source->video.profile_level_id[0] &&
        profile && profile_cap) {
        snprintf(profile, profile_cap, "%s", s->source->video.profile_level_id);
    }
    if (webrtc_h264_extract_ps(s, &sps, &sps_len, &pps, &pps_len)) {
        if (profile && profile_cap && (!profile[0] || strncmp(profile, "000000", 6) == 0)) {
            webrtc_h264_profile_from_sps(sps, sps_len, profile, profile_cap);
        }
        if (sps && sps_len && pps && pps_len && sprop && sprop_cap) {
            encode_b64(sps, sps_len, sps_b64, sizeof(sps_b64));
            encode_b64(pps, pps_len, pps_b64, sizeof(pps_b64));
            snprintf(sprop, sprop_cap, "%s,%s", sps_b64, pps_b64);
        }
    }
    if (profile && profile_cap && !profile[0] && s && s->source) {
        vcfg = zms_media_source_video_config(s->source, &vcfg_len);
        if (vcfg && vcfg_len && !zms_rtmp_avc_profile_level_id(vcfg, vcfg_len, profile)) {
            snprintf(profile, profile_cap, "42001f");
        }
    }
    if (profile && profile_cap && profile[0]) {
        webrtc_h264_profile_lower(profile);
    } else if (profile && profile_cap) {
        snprintf(profile, profile_cap, "42001f");
    }
    zn_info("[webrtc] h264 fmtp profile=%s sprop=%s len=%zu", profile ? profile : "?",
             (sprop && sprop[0]) ? "yes" : "no", (sprop && sprop[0]) ? strlen(sprop) : 0);
}

/*
 * 协商一条轨道。
 *
 * 播放方向且偏好明确时，先按 offer 的 payload 顺序逐个尝试“偏好 codec”，命中
 * 即用；都不中再放宽到任意可协商 codec。这种“先精确后宽松”的顺序保证：有匹配
 * 偏好就用偏好（减少转码/保证能力），没有也不至于直接失败——很多浏览器 offer
 * 的 pt 顺序与编码名和我们的偏好并不一致。
 * H264 播放走专用打分路径（webrtc_negotiate_h264_track），因为要在多个 H264 pt
 * 里按 profile/packetization 择优，不能取第一个。
 */
static int webrtc_negotiate_track(sdp_t *sdp, const char *media_type, zms_codec_id prefer,
                                  int for_play, const zms_webrtc_session *sess,
                                  webrtc_neg_track *out)
{
    int media_idx, i, n, fmts[16];
    webrtc_neg_pick_ctx pick;

    if (!sdp || !media_type || !out) {
        return -1;
    }
    if (for_play && prefer == ZMS_CODEC_H264 && strcmp(media_type, "video") == 0) {
        return webrtc_negotiate_h264_track(sdp, sess, out);
    }
    media_idx = webrtc_media_index(sdp, media_type);
    if (media_idx < 0) {
        return -1;
    }
    n = sdp_media_formats(sdp, media_idx, fmts, (int)(sizeof(fmts) / sizeof(fmts[0])));
    if (n <= 0) {
        return -1;
    }

    memset(&pick, 0, sizeof(pick));
    pick.for_play = for_play;
    pick.is_video = strcmp(media_type, "video") == 0;
    pick.prefer = prefer;
    pick.out = out;

    if (for_play && prefer != ZMS_CODEC_INVALID) {
        for (i = 0; i < n; ++i) {
            pick.done = 0;
            pick.want_pt = fmts[i];
            sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
            if (pick.done) {
                return 0;
            }
        }
        pick.done = 0;
        pick.want_pt = -1;
        sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
        if (pick.done) {
            return 0;
        }
    } else {
        pick.want_pt = -1;
        sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_rtpmap_cb, &pick);
        if (pick.done) {
            return 0;
        }
    }

    webrtc_log_media_rtpmaps(sdp, media_idx, media_type);
    zn_warn("[webrtc] negotiate failed media=%s fmts=%d", media_type, n);
    for (i = 0; i < n; ++i) {
        zn_warn("[webrtc]   fmt[%d]=%d", i, fmts[i]);
    }
    return -1;
}

static zms_codec_id webrtc_prefer_video_codec(const zms_webrtc_session *s)
{
    if (s && s->source && s->source->video.ready && s->source->video.codec != ZMS_CODEC_INVALID) {
        return s->source->video.codec;
    }
    return ZMS_CODEC_H264;
}

static zms_codec_id webrtc_prefer_audio_codec(const zms_webrtc_session *s)
{
    if (s && s->source && s->source->audio.ready && s->source->audio.codec != ZMS_CODEC_INVALID) {
        /* 仅当源流本身是 Opus/G711 时才协商播放音轨；AAC 等在 negotiate 前过滤 */
        if (s->source->audio.codec == ZMS_CODEC_OPUS ||
            s->source->audio.codec == ZMS_CODEC_G711A ||
            s->source->audio.codec == ZMS_CODEC_G711U) {
            return s->source->audio.codec;
        }
        return ZMS_CODEC_INVALID;
    }
    return ZMS_CODEC_OPUS;
}

static int webrtc_source_audio_playable(const zms_webrtc_session *s)
{
    if (!s || !s->source || !s->source->has_audio) {
        return 0;
    }
    if (s->source->audio.ready && s->source->audio.codec != ZMS_CODEC_INVALID) {
        return s->source->audio.codec == ZMS_CODEC_OPUS ||
               s->source->audio.codec == ZMS_CODEC_G711A ||
               s->source->audio.codec == ZMS_CODEC_G711U;
    }
    /* 音轨未就绪时不强行带音频，避免 AAC 源误协商 */
    return 0;
}

/** 从 offer 的 audio m-line 选择对讲下行编解码：PCMU > PCMA > Opus。 */
typedef struct webrtc_talk_pick_ctx {
    int pcmu_pt;
    int pcma_pt;
    int opus_pt;
    int opus_rate;
} webrtc_talk_pick_ctx;

static void webrtc_pick_talk_rtpmap_cb(void *param, const char *name, const char *value)
{
    webrtc_talk_pick_ctx *ctx = (webrtc_talk_pick_ctx *)param;
    int pt = 0;
    int rate = 0;
    char enc[16];
    char params[64];

    if (!ctx || !name || !value || strcmp(name, "rtpmap") != 0) {
        return;
    }
    enc[0] = params[0] = '\0';
    if (sdp_a_rtpmap(value, &pt, enc, &rate, params) != 0) {
        return;
    }
    if (webrtc_codec_from_encoding(enc) == ZMS_CODEC_G711U && ctx->pcmu_pt < 0) {
        ctx->pcmu_pt = pt;
    } else if (webrtc_codec_from_encoding(enc) == ZMS_CODEC_G711A && ctx->pcma_pt < 0) {
        ctx->pcma_pt = pt;
    } else if (webrtc_codec_from_encoding(enc) == ZMS_CODEC_OPUS && ctx->opus_pt < 0) {
        ctx->opus_pt = pt;
        ctx->opus_rate = rate > 0 ? rate : 48000;
    }
}

/*
 * 对讲（talk）下行 codec 选择：PCMU > PCMA > Opus。
 * 优先 G711 的理由：对讲链路要求极低延迟与低 CPU，G711 无需编码状态、逐样本
 * 直通最省；Opus 有帧级打包与状态，延迟更高。采样率固定 8000（G711 天然 8k）。
 */
static int webrtc_pick_talk_codec(sdp_t *sdp, int *pt, zms_codec_id *codec, int *rate)
{
    int media_idx;
    webrtc_talk_pick_ctx ctx;

    if (!sdp || !pt || !codec || !rate) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "audio");
    if (media_idx < 0) {
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.pcmu_pt = ctx.pcma_pt = ctx.opus_pt = -1;
    (void)sdp_media_attribute_list(sdp, media_idx, "rtpmap", webrtc_pick_talk_rtpmap_cb, &ctx);
    if (ctx.pcmu_pt >= 0) {
        *pt = ctx.pcmu_pt;
        *codec = ZMS_CODEC_G711U;
        *rate = 8000;
        return 0;
    }
    if (ctx.pcma_pt >= 0) {
        *pt = ctx.pcma_pt;
        *codec = ZMS_CODEC_G711A;
        *rate = 8000;
        return 0;
    }
    if (ctx.opus_pt >= 0) {
        *pt = ctx.opus_pt;
        *codec = ZMS_CODEC_OPUS;
        *rate = ctx.opus_rate;
        return 0;
    }
    return -1;
}

typedef struct webrtc_extmap_scan {
    int twcc_id;
} webrtc_extmap_scan;

static void webrtc_scan_extmap(void *param, const char *name, const char *value)
{
    webrtc_extmap_scan *scan = (webrtc_extmap_scan *)param;
    int id;

    (void)name;
    if (!scan || !value || scan->twcc_id > 0) {
        return;
    }
    if (!strstr(value, "transport-wide-cc") && !strstr(value, "transport-cc")) {
        return;
    }
    id = atoi(value);
    if (id > 0 && id <= 14) {
        scan->twcc_id = id;
    }
}

static void webrtc_session_save_layout(zms_webrtc_session *mut, sdp_t *sdp)
{
    int ai, vi;
    int i;
    int count;
    const char *mid;

    ai = webrtc_media_index(sdp, "audio");
    vi = webrtc_media_index(sdp, "video");
    mut->offer_audio_before_video = (ai >= 0 && vi >= 0 && ai < vi) ? 1 : 0;
    mut->offer_has_data = webrtc_media_index(sdp, "application") >= 0;
    mut->offer_media_count = 0;
    mut->data_mid[0] = '\0';
    count = sdp_media_count(sdp);
    if (count > (int)(sizeof(mut->offer_media_type) / sizeof(mut->offer_media_type[0]))) {
        count = (int)(sizeof(mut->offer_media_type) / sizeof(mut->offer_media_type[0]));
    }
    for (i = 0; i < count; ++i) {
        const char *type = sdp_media_type(sdp, i);

        if (!type || !type[0]) {
            continue;
        }
        snprintf(mut->offer_media_type[mut->offer_media_count],
                 sizeof(mut->offer_media_type[0]), "%s", type);
        mid = sdp_media_attribute_find(sdp, i, "mid");
        snprintf(mut->offer_media_mid[mut->offer_media_count], sizeof(mut->offer_media_mid[0]), "%s",
                 mid && mid[0] ? mid : "0");
        if (strcmp(type, "application") == 0) {
            snprintf(mut->data_mid, sizeof(mut->data_mid), "%s",
                     mid && mid[0] ? mid : mut->offer_media_mid[mut->offer_media_count]);
        }
        ++mut->offer_media_count;
    }
    if (vi >= 0) {
        mid = sdp_media_attribute_find(sdp, vi, "mid");
        snprintf(mut->video_mid, sizeof(mut->video_mid), "%s", mid && mid[0] ? mid : "0");
    } else {
        mut->video_mid[0] = '\0';
    }
    mut->rt.twcc_ext_id = 0;
    if (vi >= 0) {
        webrtc_extmap_scan scan;

        scan.twcc_id = 0;
        (void)sdp_media_attribute_list(sdp, vi, "extmap", webrtc_scan_extmap, &scan);
        mut->rt.twcc_ext_id = scan.twcc_id;
    }
    if (ai >= 0) {
        mid = sdp_media_attribute_find(sdp, ai, "mid");
        snprintf(mut->audio_mid, sizeof(mut->audio_mid), "%s", mid && mid[0] ? mid : "1");
    } else {
        mut->audio_mid[0] = '\0';
    }
}

/*
 * 协商总入口。处理顺序与几个“降级而非失败”的决策：
 *   1. 播放时若 offer 的 audio m-line 是 recvonly/inactive，说明对方不想收音频，
 *      跳过对讲协商（否则会往一个不收音频的方向发对讲）；
 *   2. 视频协商失败 → 整体失败（播放/推流没有视频没意义）；
 *   3. 音频：AAC 等浏览器不支持的源 codec 直接过滤成 video-only，而不是让整个
 *      answer 失败；播放方向的音频协商失败同样降级为 video-only。推流方向则
 *      不允许降级（推流少了音频轨是实质性问题）。
 * 电视场景常见“只有视频没音频/音频不可用”，所以音频的容错力度明显大于视频。
 */
int webrtc_session_negotiate(zms_webrtc_session *mut, sdp_t *sdp, int for_play)
{
    webrtc_neg_track v, a;

    mut->offer_has_video = webrtc_offer_has_media(sdp, "video");
    mut->offer_has_audio = webrtc_offer_has_media(sdp, "audio");
    mut->answer_has_video = mut->offer_has_video;
    mut->answer_has_audio = mut->offer_has_audio;
    mut->video_codec = ZMS_CODEC_INVALID;
    mut->audio_codec = ZMS_CODEC_INVALID;
    mut->audio_rate = 48000;
    mut->audio_channels = 2;
    mut->talk_enabled = 0;
    mut->talk_pt = 0;
    mut->talk_codec = ZMS_CODEC_INVALID;
    mut->talk_rate = 8000;

    if (for_play && mut->offer_has_audio) {
        int talk_pt = 0;
        int talk_rate = 0;
        zms_codec_id talk_codec = ZMS_CODEC_INVALID;
        int audio_idx = webrtc_media_index(sdp, "audio");
        int offer_can_send = 1;

        if (audio_idx >= 0 &&
            (sdp_media_attribute_find(sdp, audio_idx, "recvonly") != NULL ||
             sdp_media_attribute_find(sdp, audio_idx, "inactive") != NULL)) {
            offer_can_send = 0;
        }
        if (offer_can_send &&
            webrtc_pick_talk_codec(sdp, &talk_pt, &talk_codec, &talk_rate) == 0) {
            mut->talk_enabled = 1;
            mut->talk_pt = (uint8_t)talk_pt;
            mut->talk_codec = talk_codec;
            mut->talk_rate = talk_rate;
            zn_info("[webrtc] talk ready codec=%s pt=%d rate=%d app=%s stream=%s",
                     zms_codec_name(talk_codec), talk_pt, talk_rate, mut->app, mut->stream);
        }
    }

    if (mut->answer_has_video) {
        if (webrtc_negotiate_track(sdp, "video",
                                   for_play ? webrtc_prefer_video_codec(mut) : ZMS_CODEC_INVALID,
                                   for_play, mut, &v) != 0) {
            return -1;
        }
        mut->video_codec = v.codec;
        mut->video_pt = v.pt;
    }
    if (mut->answer_has_audio && for_play && !webrtc_source_audio_playable(mut)) {
        zms_codec_id src_ac = ZMS_CODEC_INVALID;
        if (mut->source && mut->source->audio.ready) {
            src_ac = mut->source->audio.codec;
        }
        zn_info("[webrtc] filter unsupported audio=%s, video-only play app=%s stream=%s",
                 zms_codec_name(src_ac), mut->app, mut->stream);
        mut->answer_has_audio = 0;
        mut->audio_codec = ZMS_CODEC_INVALID;
        mut->audio_pt = 0;
    }
    if (mut->answer_has_audio) {
        if (webrtc_negotiate_track(sdp, "audio",
                                   for_play ? webrtc_prefer_audio_codec(mut) : ZMS_CODEC_INVALID,
                                   for_play, mut, &a) != 0) {
            if (for_play) {
                zn_info("[webrtc] audio negotiate failed, video-only play app=%s stream=%s",
                         mut->app, mut->stream);
                mut->answer_has_audio = 0;
                mut->audio_codec = ZMS_CODEC_INVALID;
                mut->audio_pt = 0;
            } else {
                return -1;
            }
        } else {
            mut->audio_codec = a.codec;
            mut->audio_pt = a.pt;
            mut->audio_rate = a.rate > 0 ? a.rate : 48000;
            mut->audio_channels = a.channels > 0 ? a.channels : 2;
        }
    }
    webrtc_session_save_layout(mut, sdp);
    return 0;
}

int webrtc_publish_store_offer_h264_sprop(zms_webrtc_session *s, sdp_t *sdp)
{
    int media_idx;
    const char *fmtp;
    const char *params;
    const char *sprop;
    char sps_b64[384];
    char pps_b64[384];
    char *comma;

    if (!s || !sdp || s->video_codec != ZMS_CODEC_H264) {
        return -1;
    }
    media_idx = webrtc_media_index(sdp, "video");
    if (media_idx < 0) {
        return -1;
    }
    fmtp = webrtc_sdp_fmtp_for_pt(sdp, media_idx, s->video_pt ? s->video_pt : 96);
    if (!fmtp) {
        return -1;
    }
    params = fmtp;
    if (atoi(fmtp) == (s->video_pt ? s->video_pt : 96)) {
        const char *sp = strchr(fmtp, ' ');
        if (sp && sp[1]) {
            params = sp + 1;
        }
    }
    sprop = strstr(params, "sprop-parameter-sets=");
    if (!sprop) {
        return -1;
    }
    sprop += 21;
    snprintf(sps_b64, sizeof(sps_b64), "%s", sprop);
    comma = strchr(sps_b64, ';');
    if (comma) {
        *comma = '\0';
    }
    comma = strchr(sps_b64, ',');
    if (!comma) {
        return -1;
    }
    *comma = '\0';
    snprintf(pps_b64, sizeof(pps_b64), "%s", comma + 1);
    s->whip_h264_sps_len =
        decode_b64(sps_b64, s->whip_h264_sps, sizeof(s->whip_h264_sps));
    s->whip_h264_pps_len =
        decode_b64(pps_b64, s->whip_h264_pps, sizeof(s->whip_h264_pps));
    if (!s->whip_h264_sps_len || !s->whip_h264_pps_len) {
        return -1;
    }
    s->whip_h264_have_sprop = 1;
    return 0;
}

void zms_webrtc_publish_apply_offer_h264_sprop(zms_webrtc_session *s)
{
    if (!s || !s->ingest || !s->whip_h264_have_sprop) {
        return;
    }
    if (zms_live_ingest_set_h264_sps_pps(s->ingest, s->whip_h264_sps, s->whip_h264_sps_len,
                                         s->whip_h264_pps, s->whip_h264_pps_len) == ZN_OK) {
        zn_info("[webrtc] WHIP applied offer sprop SPS=%zu PPS=%zu app=%s stream=%s",
                 s->whip_h264_sps_len, s->whip_h264_pps_len, s->app, s->stream);
    }
}

int webrtc_offer_setup_passive(sdp_t *sdp)
{
    const char *v;
    int i, n;

    if (!sdp) {
        return 0;
    }
    v = sdp_attribute_find(sdp, "setup");
    if (v && v[0]) {
        return webrtc_strcasecmp(v, "passive") == 0;
    }
    n = sdp_media_count(sdp);
    for (i = 0; i < n; ++i) {
        v = sdp_media_attribute_find(sdp, i, "setup");
        if (v && v[0]) {
            return webrtc_strcasecmp(v, "passive") == 0;
        }
    }
    return 0;
}
