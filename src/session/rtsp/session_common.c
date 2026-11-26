/**
 * @file session_common.c
 * @brief RTSP URL/轨道解析与 SETUP 匹配等公共辅助。
 */
#include "zms/play/vod_play_port.h"
#include "session/rtsp/session_internal.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/clock.h"
#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/codec_id.h"
#include "aom-av1.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include "zms/util/base64.h"
#include <string.h>

/** SDP 侧只需要长度：失败时返回已处理部分。 */
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

/*
 * 视频编解码器判定，三级回退：
 *   1. 源已就绪（已解出 SPS/配置）时以源声明为准，最可靠；
 *   2. 否则尝试从 config 字节里嗅探（推流早于媒体源就绪时走这条）；
 *   3. 都不行就默认 H264——绝大多数摄像机/推流端是 H264，默认它比报错更实用。
 */
static zms_codec_id rtsp_sdp_video_codec(const zms_media_source *src, const uint8_t *vcfg,
                                         size_t vcfg_len)
{
    if (src && src->video.ready && src->video.codec != ZMS_CODEC_INVALID) {
        return src->video.codec;
    }
    if (vcfg && vcfg_len > 0) {
        zms_codec_id vc = zms_flv_video_config_codec(vcfg, vcfg_len);
        if (vc != ZMS_CODEC_INVALID) {
            return vc;
        }
    }
    return ZMS_CODEC_H264;
}

static const char *rtsp_sdp_video_encoding(zms_codec_id vc)
{
    const zms_codec_meta *meta = zms_codec_meta_get(vc);

    return (meta && meta->sdp_mime[0]) ? meta->sdp_mime : "H264";
}

static void rtsp_sdp_load_video_sprop(zms_codec_id vc, const uint8_t *vcfg, size_t vcfg_len,
                                      char *profile, size_t profile_cap, char *vps_b64,
                                      size_t vps_cap, char *sps_b64, size_t sps_cap, char *pps_b64,
                                      size_t pps_cap)
{
    if (profile && profile_cap) {
        profile[0] = '\0';
    }
    if (vps_b64 && vps_cap) {
        vps_b64[0] = '\0';
    }
    if (sps_b64 && sps_cap) {
        sps_b64[0] = '\0';
    }
    if (pps_b64 && pps_cap) {
        pps_b64[0] = '\0';
    }

    if (vc == ZMS_CODEC_H265 && vcfg && vcfg_len > 0) {
        const uint8_t *vps = NULL, *sps = NULL, *pps = NULL;
        size_t vps_len = 0, sps_len = 0, pps_len = 0;

        if (zms_h265_video_config_param_sets(vcfg, vcfg_len, &vps, &vps_len, &sps, &sps_len, &pps,
                                             &pps_len)) {
            if (vps && vps_len) {
                encode_b64(vps, vps_len, vps_b64, vps_cap);
            }
            encode_b64(sps, sps_len, sps_b64, sps_cap);
            encode_b64(pps, pps_len, pps_b64, pps_cap);
        }
        return;
    }

    if (vcfg && vcfg_len > 0 && vc == ZMS_CODEC_H264) {
        const uint8_t *sps = NULL, *pps = NULL;
        size_t sps_len = 0, pps_len = 0;

        if (zms_rtmp_avc_extract_sps_pps(vcfg, vcfg_len, &sps, &sps_len, &pps, &pps_len)) {
            encode_b64(sps, sps_len, sps_b64, sps_cap);
            encode_b64(pps, pps_len, pps_b64, pps_cap);
            if (profile && profile_cap && !zms_rtmp_avc_profile_level_id(vcfg, vcfg_len, profile)) {
                snprintf(profile, profile_cap, "42001f");
            }
        }
    }
}

void zms_rtsp_url_vhost(const char *url, char *out, size_t cap)
{
    zms_vhost_normalize(url, out, cap);
}

void zms_rtsp_session_parse_url(const char *url, char *app, char *stream)
{
    zms_media_split_path(url, app, stream);
}

void zms_rtsp_session_capture_params(zms_rtsp_session *rs, const char *url)
{
    char params[ZMS_AUTH_PARAMS_MAX];

    if (!rs || !url) {
        return;
    }
    zms_auth_split_query(url, NULL, 0, params, sizeof(params));
    if (params[0]) {
        zms_auth_params_copy(params, rs->params, sizeof(rs->params));
    }
}

/*
 * 判断请求里的 Session 头是否属于本会话。
 *
 * 空 session_hdr 的语义：带空头的请求只在“会话尚未分配 ID”时匹配（如首个
 * SETUP 后的 DESCRIBE 复用连接）；此时 rs->session_id 非空则拒绝。
 * 反之，已分配 ID 后必须严格相等，末尾的 ";timeout=xx" 等参数先剥离再比。
 */
int zms_rtsp_session_match(zms_rtsp_session *rs, const char *session_hdr)
{
    if (!session_hdr || !session_hdr[0]) {
        return rs->session_id[0] == '\0';
    }
    if (!rs->session_id[0]) {
        return 1;
    }
    char sid[64];
    strncpy(sid, session_hdr, sizeof(sid) - 1);
    sid[sizeof(sid) - 1] = '\0';
    char *semi = strchr(sid, ';');
    if (semi) {
        *semi = '\0';
    }
    return strcmp(sid, rs->session_id) == 0;
}

static const char *find_track_id_param(const char *url)
{
    static const char *keys[] = {"trackID=", "trackid=", "streamid=", "streamID="};
    if (!url) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const char *p = strstr(url, keys[i]);
        if (p) {
            return p + strlen(keys[i]);
        }
    }
    return NULL;
}

/*
 * 从 SETUP URL 解出要建立的是哪条轨道。
 *
 * 优先看 trackID/streamid 查询参数：不同客户端（VLC、GStreamer、部分 IPC）
 * 各用各的参数名，故逐个尝试。没有查询参数时回退到 SDP 的 a=control 值匹配
 * （如 "trackID=1" 这类整串子串命中），仍找不到就当 0 号轨。
 */
int zms_rtsp_session_setup_track_id(const char *url, const zms_sdp_session *sdp)
{
    if (!url) {
        return 0;
    }
    const char *tid = find_track_id_param(url);
    if (tid) {
        return atoi(tid);
    }
    if (sdp) {
        for (unsigned i = 0; i < sdp->track_count; ++i) {
            const char *ctrl = sdp->tracks[i].control;
            if (ctrl[0] && strstr(url, ctrl)) {
                return (int)i;
            }
        }
    }
    return 0;
}

int zms_rtsp_session_record_track_by_channel(const zms_rtsp_session *rs, uint8_t channel)
{
    if (!rs) {
        return -1;
    }
    for (unsigned i = 0; i < rs->pb.publish_sdp.track_count; ++i) {
        if (rs->pb.publish_sdp.tracks[i].interleaved_rtp == channel) {
            return (int)i;
        }
    }
    return -1;
}

/* 是否已就绪可 RECORD：任一轨道已 SETUP 即视为可推流。
 * 特例：SDP 里没有轨道（track_count==0）时也返回 1，因为无轨道的推流请求
 * 不应被卡在“还要再 SETUP”的状态里空等。 */
int zms_rtsp_session_record_any_track_setup(const zms_rtsp_session *rs)
{
    if (!rs) {
        return 0;
    }
    for (unsigned i = 0; i < rs->pb.publish_sdp.track_count; ++i) {
        if (rs->pb.publish_track_setup[i]) {
            return 1;
        }
    }
    return rs->pb.publish_sdp.track_count == 0;
}

uint64_t zms_rtsp_session_vod_duration_ms(const zms_rtsp_session *rs)
{
    uint64_t dur_ms = 0;

    if (!rs || !rs->source || !zms_media_source_is_vod(rs->source)) {
        return 0;
    }
    dur_ms = zms_vod_source_duration_ms(rs->source);
    if (!dur_ms) {
        dur_ms = zms_vod_probe_duration_ms(rs->app, rs->stream);
    }
    return dur_ms;
}

/*
 * 点播播放进度（毫秒）。
 *
 * 有 muxer 且已出过包时用 RTP 时间戳反算真实播放位置（比 seek_ms 精确，
 * 因为它反映“已经发出去多少媒体时间”）；否则退回到本会话记录的 seek_ms。
 * 时钟频率缺省值：视频 90000（RTP 视频标准），音频取采样率、再退 48000。
 */
uint64_t zms_rtsp_session_vod_play_position_ms(const zms_rtsp_session *rs)
{
    const zms_rtp_muxer_stats *st;
    uint32_t vhz;
    uint32_t ahz;

    if (!rs) {
        return 0;
    }
    if (!rs->pl.rtp_muxer || !rs->source || !zms_media_source_is_vod(rs->source)) {
        return rs->pl.seek_ms;
    }
    st = zms_rtp_muxer_get_stats(rs->pl.rtp_muxer);
    if (!st) {
        return rs->pl.seek_ms;
    }
    vhz = rs->pb.video_clock_hz > 0 ? rs->pb.video_clock_hz : 90000u;
    if (rs->source->has_video && st->video_pkt_count > 0) {
        return zms_rtp_clock_to_ms(st->video_last_rtp_ts, vhz);
    }
    ahz = rs->pb.audio_clock_hz > 0 ? rs->pb.audio_clock_hz
                                 : (uint32_t)(rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 48000);
    if (rs->source->has_audio && st->audio_pkt_count > 0) {
        return zms_rtp_clock_to_ms(st->audio_last_rtp_ts, ahz);
    }
    return rs->pl.seek_ms;
}

/*
 * 载入音频参数。先写一套保守默认值（AAC/44100/双声道）再按源信息逐项覆盖，
 * 这样即使源没准备好，SDP 里也总有合法的 a=rtpmap 可用，不会生成残缺描述。
 *
 * 三档来源：
 *   1. source->audio.ready：直接用已解出的参数；
 *   2. 仅有 audio config 字节：现解 ASC（AAC 的 AudioSpecificConfig 在第 2
 *      字节起，前 2 字节是 FLV AudioTagHeader），或按 codec 推默认；
 *   3. 都没有：保留前面的默认值。
 * audio_clock_hz 取采样率本身——音频 RTP 时钟就是采样率，不是固定 90k。
 */
void zms_rtsp_session_load_audio_params(zms_rtsp_session *rs)
{
    rs->pb.audio_rate = 44100;
    rs->pb.audio_channels = 2;
    rs->pb.audio_codec = ZMS_CODEC_AAC;
    if (!rs->source) {
        return;
    }
    if (rs->source->audio.ready) {
        rs->pb.audio_codec = rs->source->audio.codec;
        rs->pb.audio_rate = rs->source->audio.sample_rate > 0 ? rs->source->audio.sample_rate : 44100;
        rs->pb.audio_channels = rs->source->audio.channels > 0 ? rs->source->audio.channels : 2;
        rs->pb.audio_clock_hz = (uint32_t)(rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 44100);
        rs->pb.video_clock_hz = 90000;
        return;
    }
    size_t clen = 0;
    const uint8_t *acfg = zms_media_source_audio_config(rs->source, &clen);
    if (acfg && clen > 0) {
        rs->pb.audio_codec = zms_flv_tag_audio_codec(acfg, clen);
    }
    if (acfg && clen > 2 && rs->pb.audio_codec == ZMS_CODEC_AAC) {
        zms_aac_parse_asc(acfg + 2, clen - 2, &rs->pb.audio_rate, &rs->pb.audio_channels);
    }
    if (rs->pb.audio_codec == ZMS_CODEC_G711A || rs->pb.audio_codec == ZMS_CODEC_G711U) {
        rs->pb.audio_rate = rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 8000;
        rs->pb.audio_channels = 1;
    }
    rs->pb.audio_clock_hz = (uint32_t)(rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 44100);
    rs->pb.video_clock_hz = 90000;
}

void zms_rtsp_session_build_sdp(zms_rtsp_session *rs, char *out, size_t cap)
{
    zms_rtsp_session_load_audio_params(rs);
    size_t vcfg_len = 0, acfg_len = 0;
    const uint8_t *vcfg = NULL;
    const uint8_t *acfg = NULL;
    char vps_b64[512], sps_b64[512], pps_b64[512], asc_hex[128];
    char profile[16] = "42001f";
    char vfmtp[2048];
    char vfmtp_line[2200];
    zms_codec_id video_codec = ZMS_CODEC_H264;
    const char *venc = "H264";
    asc_hex[0] = '\0';
    vps_b64[0] = sps_b64[0] = pps_b64[0] = '\0';
    vfmtp[0] = '\0';

    if (rs->source) {
        vcfg = zms_media_source_video_config(rs->source, &vcfg_len);
        acfg = zms_media_source_audio_config(rs->source, &acfg_len);
    }

    if (rs->source && rs->source->video.ready && rs->source->video.profile_level_id[0]) {
        strncpy(profile, rs->source->video.profile_level_id, sizeof(profile) - 1);
    }
    if (rs->source && rs->source->audio.ready && rs->source->audio.asc_hex[0]) {
        strncpy(asc_hex, rs->source->audio.asc_hex, sizeof(asc_hex) - 1);
    }

    video_codec = rtsp_sdp_video_codec(rs->source, vcfg, vcfg_len);
    venc = rtsp_sdp_video_encoding(video_codec);
    rtsp_sdp_load_video_sprop(video_codec, vcfg, vcfg_len, profile, sizeof(profile), vps_b64,
                              sizeof(vps_b64), sps_b64, sizeof(sps_b64), pps_b64, sizeof(pps_b64));

    if (video_codec == ZMS_CODEC_H265) {
        snprintf(vfmtp, sizeof(vfmtp), "sprop-vps=%s;sprop-sps=%s;sprop-pps=%s", vps_b64, sps_b64,
                 pps_b64);
        if (!sps_b64[0] || !pps_b64[0]) {
            if (rs->source && rs->source->has_video) {
                zn_warn("RTSP SDP: missing H265 sprop for %s/%s (vcfg_len=%u)", rs->app,
                         rs->stream, (unsigned)vcfg_len);
            }
        }
    } else if (video_codec == ZMS_CODEC_H264) {
        snprintf(vfmtp, sizeof(vfmtp),
                 "packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,%s", profile,
                 sps_b64, pps_b64);
        if (!sps_b64[0] || !pps_b64[0]) {
            if (rs->source && rs->source->has_video) {
                zn_warn("RTSP SDP: missing AVC config for %s/%s (vcfg_len=%u)", rs->app,
                         rs->stream, (unsigned)vcfg_len);
            }
        }
    } else if (video_codec == ZMS_CODEC_AV1 && vcfg && vcfg_len > 0) {
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;
        struct aom_av1_t av1;

        memset(&av1, 0, sizeof(av1));
        if (zms_av1_over_rtmp_config_extradata(vcfg, vcfg_len, &av1c, &av1c_len) && av1c &&
            av1c_len >= 4 && aom_av1_codec_configuration_record_load(av1c, av1c_len, &av1) > 0) {
            snprintf(vfmtp, sizeof(vfmtp), "profile=%u;level-idx=%u;tier=%u",
                     (unsigned)av1.seq_profile, (unsigned)av1.seq_level_idx_0,
                     (unsigned)av1.seq_tier_0);
        }
    } else {
        vfmtp[0] = '\0';
    }
    if (vfmtp[0]) {
        snprintf(vfmtp_line, sizeof(vfmtp_line), "a=fmtp:96 %s\r\n", vfmtp);
    } else {
        vfmtp_line[0] = '\0';
    }

/* 把 AAC 的 audio config 里 ASC（跳过 2 字节 FLV 头）转成 SDP 要的 hex 串。 */
    if (!asc_hex[0] && acfg && acfg_len > 2) {
        char *p = asc_hex;
        for (size_t i = 2; i < acfg_len && (size_t)(p - asc_hex) < sizeof(asc_hex) - 3; ++i) {
            p += snprintf(p, (size_t)(asc_hex + sizeof(asc_hex) - p), "%02x", acfg[i]);
        }
    }

    {
        /*
         * 按音频 codec 选 SDP 模板。分支顺序有讲究：
         *   - G711 优先：它不需要 config 字节，最稳，先命中；
         *   - 其次 AAC：需要有效 ASC（asc_hex），否则播放端解不出采样率；
         *   - 再 OPUS：WebRTC 常见，固定 48k；
         *   - 兜底：只发视频轨（源有音频但不支持的 codec 时宁可不发音频，
         *     也不发一个播放端无法解码的 m=audio）。
         */
        double dur_sec = 0.0;
        char range_line[80];
        if (rs->source && zms_media_source_is_vod(rs->source)) {
            dur_sec = zms_rtsp_session_vod_duration_ms(rs) / 1000.0;
            if (dur_sec > 0.0) {
                snprintf(range_line, sizeof(range_line), "a=range:npt=0-%.3f\r\n", dur_sec);
            } else {
                snprintf(range_line, sizeof(range_line), "a=range:npt=0-\r\n");
            }
        } else {
            snprintf(range_line, sizeof(range_line), "a=range:npt=now-\r\n");
        }

        /* SDP o= 用对外公告地址（advertise_host）；无则回退 127.0.0.1 */
        const char *sdp_origin = (rs->advertise_host[0]) ? rs->advertise_host : "127.0.0.1";

        if (rs->pb.audio_codec == ZMS_CODEC_G711A || rs->pb.audio_codec == ZMS_CODEC_G711U) {
            const char *enc = rs->pb.audio_codec == ZMS_CODEC_G711U ? "PCMU" : "PCMA";
            unsigned apt = zms_g711_over_rtp_default_pt(rs->pb.audio_codec);
            int ar = rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 8000;
            snprintf(out, cap,
                     "v=0\r\n"
                     "o=- 0 0 IN IP4 %s\r\n"
                     "s=ZMS\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "t=0 0\r\n"
                     "%s"
                     "a=control:*\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 %s/90000\r\n"
                     "%s"
                     "a=control:trackID=0\r\n"
                     "m=audio 0 RTP/AVP %u\r\n"
                     "a=rtpmap:%u %s/%d/1\r\n"
                     "a=control:trackID=1\r\n",
                     sdp_origin, range_line, venc, vfmtp_line, apt, apt, enc, ar);
        } else if (acfg_len > 2 && asc_hex[0] && rs->pb.audio_codec == ZMS_CODEC_AAC) {
            snprintf(out, cap,
                     "v=0\r\n"
                     "o=- 0 0 IN IP4 %s\r\n"
                     "s=ZMS\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "t=0 0\r\n"
                     "%s"
                     "a=control:*\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 %s/90000\r\n"
                     "%s"
                     "a=control:trackID=0\r\n"
                     "m=audio 0 RTP/AVP 97\r\n"
                     "a=rtpmap:97 mpeg4-generic/%d/%d\r\n"
                     "a=fmtp:97 "
                     "profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength="
                     "3;config=%s\r\n"
                     "a=control:trackID=1\r\n",
                     sdp_origin, range_line, venc, vfmtp_line, rs->pb.audio_rate, rs->pb.audio_channels,
                     asc_hex);
        } else if (rs->source && rs->source->has_audio && rs->pb.audio_codec == ZMS_CODEC_OPUS) {
            int ar = rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 48000;
            int ch = rs->pb.audio_channels > 0 ? rs->pb.audio_channels : 2;
            snprintf(out, cap,
                     "v=0\r\n"
                     "o=- 0 0 IN IP4 %s\r\n"
                     "s=ZMS\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "t=0 0\r\n"
                     "%s"
                     "a=control:*\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 %s/90000\r\n"
                     "%s"
                     "a=control:trackID=0\r\n"
                     "m=audio 0 RTP/AVP 97\r\n"
                     "a=rtpmap:97 opus/%d/%d\r\n"
                     "a=fmtp:97 minptime=10;useinbandfec=1\r\n"
                     "a=control:trackID=1\r\n",
                     sdp_origin, range_line, venc, vfmtp_line, ar, ch);
        } else {
            snprintf(out, cap,
                     "v=0\r\n"
                     "o=- 0 0 IN IP4 %s\r\n"
                     "s=ZMS\r\n"
                     "c=IN IP4 0.0.0.0\r\n"
                     "t=0 0\r\n"
                     "%s"
                     "a=control:*\r\n"
                     "m=video 0 RTP/AVP 96\r\n"
                     "a=rtpmap:96 %s/90000\r\n"
                     "%s"
                     "a=control:trackID=0\r\n",
                     sdp_origin, range_line, venc, vfmtp_line);
        }
    }
}

void zms_rtsp_session_send_resp(zms_rtsp_session *rs, int code, const char *reason,
                                const char *extra, const char *body, size_t body_len)
{
    size_t n = 0;
    const char *session = rs->session_id[0] ? rs->session_id : NULL;
    size_t need = ZMS_MEDIA_IO_BUF_SIZE;

    /* extra 已含 Session 头，跳过重复 */
    /* 否则响应里会出现两个 Session 头，部分客户端会因此拒绝整条响应。 */
    if (extra && session && strstr(extra, "Session:")) {
        session = NULL;
    }
    if (!rs) {
        return;
    }
    if (rs->poller) {
        if (!zms_buf_pool_slot_resize_poller(&rs->resp_buf, &rs->resp_cap, need, rs->poller)) {
            return;
        }
    } else if (!zms_buf_pool_slot_resize(&rs->resp_buf, &rs->resp_cap, need)) {
        return;
    }
    if (zms_rtsp_message_build_response(rs->resp_buf, rs->resp_cap, code, reason, rs->cseq, session,
                                        extra, body, body_len, &n) == ZN_OK) {
        zn_tcp_session_send(rs->tcp, rs->resp_buf, n);
    }
}

/*
 * 以 TCP 交织方式发送一个 RTP 包（RTSP over TCP）。
 * 帧头 "$" + 1 字节 channel + 2 字节大端长度；长度字段是 16 位，故单包
 * 不能超过 65535（RTP 本身受 MTU 约束，正常不会触顶）。
 *
 * 正在关闭/销毁途中不再写：socket 可能已被对端半关或即将释放，此时写会
 * 报错甚至踩到悬空指针；静默丢弃比制造噪音日志更合适。
 */
void zms_rtsp_session_send_rtp_interleaved(zms_rtsp_session *rs, uint8_t channel,
                                           const uint8_t *rtp, size_t rtp_len)
{
    uint8_t hdr[4];
    const void *parts[2];
    size_t lens[2];

    if (!rs || rs->close_pending || rs->destroy_scheduled || !rs->tcp || !rtp || rtp_len == 0) {
        return;
    }
    hdr[0] = '$';
    hdr[1] = channel;
    hdr[2] = (uint8_t)((rtp_len >> 8) & 0xff);
    hdr[3] = (uint8_t)(rtp_len & 0xff);
    parts[0] = hdr;
    lens[0] = 4;
    parts[1] = rtp;
    lens[1] = rtp_len;
    (void)zn_tcp_session_sendv(rs->tcp, parts, lens, 2);
}

void zms_rtsp_session_write_interleaved(void *user, uint8_t channel, const uint8_t *payload,
                                        size_t len)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    zms_rtsp_session_send_media(rs, channel, payload, len);
}
