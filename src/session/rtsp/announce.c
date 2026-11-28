/**
 * @file announce.c
 * @brief RTSP ANNOUNCE：解析推流 SDP 并挂接入站。
 */
#include "session/rtsp/session_internal.h"
#include "zcs/core/gop/queue.h"
#include "zcs/protocol/rtsp/sdp.h"
#include "zcs/util/base64.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>

/** 返回解出的字节数；缓冲不足时返回已写入部分。 */
static size_t decode_b64(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    (void)zcs_base64_decode(in, out, cap, &n);
    return n;
}

static void trim_fmtp_token(char *s)
{
    if (!s) {
        return;
    }
    for (char *p = s; *p; ++p) {
        if (*p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
}

static void apply_h264_sprop(zcs_rtsp_session *rs, const char *fmtp)
{
    if (!rs || !rs->pb.ingress || !fmtp) {
        return;
    }
    const char *sprop = strstr(fmtp, "sprop-parameter-sets=");
    if (!sprop) {
        return;
    }
    sprop += 21;
    const char *comma = strchr(sprop, ',');
    if (!comma) {
        return;
    }

    char sps_b64[384], pps_b64[384];
    size_t slen = (size_t)(comma - sprop);
    if (slen >= sizeof(sps_b64)) {
        slen = sizeof(sps_b64) - 1;
    }
    memcpy(sps_b64, sprop, slen);
    sps_b64[slen] = '\0';
    trim_fmtp_token(sps_b64);
    strncpy(pps_b64, comma + 1, sizeof(pps_b64) - 1);
    pps_b64[sizeof(pps_b64) - 1] = '\0';
    trim_fmtp_token(pps_b64);

    uint8_t sps[256], pps[256];
    size_t sps_len = decode_b64(sps_b64, sps, sizeof(sps));
    size_t pps_len = decode_b64(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }

    (void)zcs_live_ingest_set_h264_sps_pps(rs->pb.ingress, sps, sps_len, pps, pps_len);
    zn_info("RTSP ANNOUNCE: applied H264 sprop SPS=%u PPS=%u", (unsigned)sps_len,
             (unsigned)pps_len);
}

static int fmtp_extract_b64(const char *fmtp, const char *key, char *out, size_t cap)
{
    char needle[32];
    const char *p;
    const char *end;
    size_t n;

    if (!fmtp || !key || !out || cap == 0) {
        return 0;
    }
    snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(fmtp, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    end = p;
    while (*end && *end != ';' && *end != ' ' && *end != '\r' && *end != '\n') {
        ++end;
    }
    n = (size_t)(end - p);
    if (n == 0 || n >= cap) {
        return 0;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    trim_fmtp_token(out);
    return out[0] != '\0';
}

static void apply_h265_sprop(zcs_rtsp_session *rs, const char *fmtp)
{
    char vps_b64[512], sps_b64[512], pps_b64[512];
    uint8_t vps[512], sps[512], pps[512];
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    if (!rs || !rs->pb.ingress || !fmtp) {
        return;
    }
    vps_b64[0] = sps_b64[0] = pps_b64[0] = '\0';
    (void)fmtp_extract_b64(fmtp, "sprop-vps", vps_b64, sizeof(vps_b64));
    if (!fmtp_extract_b64(fmtp, "sprop-sps", sps_b64, sizeof(sps_b64))) {
        return;
    }
    if (!fmtp_extract_b64(fmtp, "sprop-pps", pps_b64, sizeof(pps_b64))) {
        return;
    }
    if (vps_b64[0]) {
        vps_len = decode_b64(vps_b64, vps, sizeof(vps));
    }
    sps_len = decode_b64(sps_b64, sps, sizeof(sps));
    pps_len = decode_b64(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }
    if (zcs_live_ingest_set_h265_vps_sps_pps(rs->pb.ingress, vps_len ? vps : NULL, vps_len, sps,
                                             sps_len, pps, pps_len) == ZN_OK) {
        zn_info("RTSP ANNOUNCE: applied H265 sprop VPS=%u SPS=%u PPS=%u", (unsigned)vps_len,
                 (unsigned)sps_len, (unsigned)pps_len);
    }
}

static void apply_announce_sdp(zcs_rtsp_session *rs, const char *sdp, size_t sdp_len)
{
    if (!rs) {
        return;
    }
    memset(&rs->pb.publish_sdp, 0, sizeof(rs->pb.publish_sdp));
    memset(rs->pb.publish_track_setup, 0, sizeof(rs->pb.publish_track_setup));
    rs->pb.video_clock_hz = 90000;
    rs->pb.audio_clock_hz = 44100;
    if (!sdp || !sdp_len || zcs_sdp_parse(sdp, sdp_len, &rs->pb.publish_sdp) != ZN_OK) {
        zn_warn("RTSP ANNOUNCE: SDP parse failed app=%s stream=%s", rs->app, rs->stream);
        return;
    }

    for (unsigned i = 0; i < rs->pb.publish_sdp.track_count; ++i) {
        const zcs_media_track *t = &rs->pb.publish_sdp.tracks[i];
        if (t->type == ZCS_TRACK_VIDEO) {
            if (rs->source) {
                rs->source->has_video = 1;
                rs->source->video.codec = t->codec;
                rs->source->video.ready = 1;
            }
            rs->pb.video_clock_hz = (uint32_t)(t->sample_rate > 0 ? t->sample_rate : 90000);
            if (t->codec != ZCS_CODEC_H264 && t->codec != ZCS_CODEC_H265 &&
                t->codec != ZCS_CODEC_AV1 && t->codec != ZCS_CODEC_VP8 &&
                t->codec != ZCS_CODEC_VP9 && t->codec != ZCS_CODEC_H266) {
                zn_warn("RTSP ANNOUNCE: unsupported video codec=%d", (int)t->codec);
                continue;
            }
            if (t->fmtp[0]) {
                if (t->codec == ZCS_CODEC_H265) {
                    apply_h265_sprop(rs, t->fmtp);
                } else if (t->codec == ZCS_CODEC_H264) {
                    apply_h264_sprop(rs, t->fmtp);
                }
            }
            if (t->interleaved_rtp == 0) {
                rs->video_rtp_ch = (uint8_t)(i * 2);
            }
        } else if (t->type == ZCS_TRACK_AUDIO) {
            if (rs->source) {
                rs->source->has_audio = 1;
            }
            rs->pb.audio_rate = t->sample_rate > 0 ? t->sample_rate : 44100;
            rs->pb.audio_channels = t->channels > 0 ? t->channels : 2;
            if (t->codec == ZCS_CODEC_G711A || t->codec == ZCS_CODEC_G711U) {
                rs->pb.audio_codec = t->codec;
                rs->pb.audio_rate = t->sample_rate > 0 ? t->sample_rate : 8000;
                rs->pb.audio_channels = t->channels > 0 ? t->channels : 1;
                rs->pb.audio_clock_hz = (uint32_t)rs->pb.audio_rate;
            } else if (t->codec == ZCS_CODEC_OPUS) {
                rs->pb.audio_codec = ZCS_CODEC_OPUS;
                rs->pb.audio_rate = t->sample_rate > 0 ? t->sample_rate : 48000;
                rs->pb.audio_clock_hz = (uint32_t)rs->pb.audio_rate;
            } else if (t->codec != ZCS_CODEC_AAC) {
                zn_warn("RTSP ANNOUNCE: unsupported audio codec=%d", (int)t->codec);
                continue;
            } else {
                rs->pb.audio_codec = ZCS_CODEC_AAC;
                rs->pb.audio_clock_hz = (uint32_t)(t->sample_rate > 0 ? t->sample_rate : 44100);
            }
            if (t->codec == ZCS_CODEC_AAC && (t->type == ZCS_TRACK_AUDIO && t->fmtp[0])) {
                const char *cfg = strstr(t->fmtp, "config=");
                if (!cfg) {
                    cfg = strstr(t->fmtp, "Config=");
                }
                if (cfg && rs->pb.ingress) {
                    char hex[128];
                    strncpy(hex, cfg + 7, sizeof(hex) - 1);
                    hex[sizeof(hex) - 1] = '\0';
                    trim_fmtp_token(hex);
                    if (hex[0]) {
                        (void)zcs_live_ingest_set_aac_config_hex(rs->pb.ingress, hex);
                        zcs_live_ingest_mark_real_audio(rs->pb.ingress);
                    }
                }
            }
            if (rs->source && rs->pb.audio_codec != ZCS_CODEC_INVALID) {
                rs->source->audio.codec = rs->pb.audio_codec;
                rs->source->audio.sample_rate = rs->pb.audio_rate;
                rs->source->audio.channels = rs->pb.audio_channels;
                rs->source->audio.ready = 1;
            }
            if (t->interleaved_rtp == 0 && i > 0) {
                rs->audio_rtp_ch = (uint8_t)(i * 2);
            } else if (i == 1) {
                rs->audio_rtp_ch = 2;
            }
        }
        zn_info("RTSP ANNOUNCE track[%u]: type=%d codec=%d pt=%d rate=%d control=%s", i,
                 (int)t->type, (int)t->codec, t->payload_type, t->sample_rate, t->control);
    }
    if (rs->pb.ingress) {
        zcs_live_ingest_set_rtp_clocks(rs->pb.ingress, rs->pb.video_clock_hz, rs->pb.audio_codec,
                                       rs->pb.audio_clock_hz);
    }
    if (rs->pb.ingress && rs->source && rs->source->has_audio && rs->pb.audio_codec == ZCS_CODEC_AAC) {
        (void)zcs_live_ingest_ensure_aac_config(rs->pb.ingress, rs->pb.audio_rate, rs->pb.audio_channels);
        zcs_live_ingest_mark_real_audio(rs->pb.ingress);
    }
}

void zcs_rtsp_session_handle_announce(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    zcs_rtsp_session_parse_url(zcs_rtsp_message_url(msg), rs->app, rs->stream);
    zcs_rtsp_session_capture_params(rs, zcs_rtsp_message_url(msg));
    zcs_rtsp_url_vhost(zcs_rtsp_message_url(msg), rs->vhost, sizeof(rs->vhost));
    if (!rs->app[0] || !rs->stream[0]) {
        zcs_rtsp_session_send_resp(rs, 403, "Forbidden", "Content-Type: text/plain\r\n",
                                   "invalid rtsp push url", 20);
        return;
    }
    char client_stream[ZCS_STREAM_MAX];
    strncpy(client_stream, rs->stream, sizeof(client_stream) - 1);
    rs->pb.ingress = zcs_live_ingest_create_publish(rs->app, rs->stream, NULL);
    if (rs->pb.ingress) {
        zcs_live_ingest_set_poller(rs->pb.ingress, rs->poller);
    }
    rs->source = rs->pb.ingress ? zcs_live_ingest_source(rs->pb.ingress) : NULL;
    if (!rs->source || !rs->source->gop_queue) {
        zcs_live_ingest_destroy(rs->pb.ingress);
        rs->pb.ingress = NULL;
        zcs_rtsp_session_send_resp(rs, 500, "Internal Server Error", NULL, NULL, 0);
        return;
    }
    if (zcs_rtsp_message_body(msg) && zcs_rtsp_message_body_len(msg)) {
        apply_announce_sdp(rs, zcs_rtsp_message_body(msg), zcs_rtsp_message_body_len(msg));
    }
    if (rs->pb.publish_sdp.track_count == 0) {
        zcs_live_ingest_destroy(rs->pb.ingress);
        rs->pb.ingress = NULL;
        rs->source = NULL;
        zcs_rtsp_session_send_resp(rs, 403, "Forbidden", "Content-Type: text/plain\r\n",
                                   "no valid track in sdp", 21);
        return;
    }
    strncpy(rs->content_base, zcs_rtsp_message_url(msg), sizeof(rs->content_base) - 1);
    if (rs->source) {
        strncpy(rs->stream, rs->source->stream, sizeof(rs->stream) - 1);
    }
    zn_info("RTSP ANNOUNCE 200: app=%s client_stream=%s stream_id=%s tracks=%u", rs->app,
             client_stream, rs->stream, rs->pb.publish_sdp.track_count);
    zcs_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}
