/**
 * @file record.c
 * @brief RTSP RECORD：入站 RTP 解包与录制到媒体源。
 */
#include "session/rtsp/session_internal.h"
#include "bridge/zm_rtp_ingest.h"
#include "bridge/zm_zms_bridge.h"
#include "zms/play/dispatch.h"
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "zms/core/event.h"
#include "zms/media/media_limits.h"
#include "zms/core/gop/limits.h"
#include "zms/media/wire/rtp_packet.h"
#include "zn/util/log.h"
#include "zn/net/tcp_server.h"
#include <stdio.h>
#include <string.h>


static int rtsp_zm_on_packet(void *user, const zm_packet *pkt)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    zms_frame frame;

    if (!rs || !rs->pb.ingress || !pkt) {
        return 0;
    }
    if (zms_zm_frame_from_packet(pkt, &frame) == ZM_OK) {
        if (frame.codec == ZMS_CODEC_H264 && !rs->pb.logged_record_h264) {
            rs->pb.logged_record_h264 = 1;
            zn_info("RTSP RECORD first ZM H264 frame: size=%u key=%d", (unsigned)frame.size,
                     frame.keyframe);
        }
        (void)zms_live_ingest_input_frame(rs->pb.ingress, &frame);
    }
    return 0;
}

static int rtsp_zm_ensure_track(zms_rtsp_session *rs, int track_idx)
{
    const zms_media_track *t;
    zms_codec_id codec;
    int clock_hz;

    if (!rs || !rs->pb.zm_rtp || track_idx < 0 ||
        (unsigned)track_idx >= rs->pb.publish_sdp.track_count) {
        return -1;
    }
    t = &rs->pb.publish_sdp.tracks[track_idx];
    codec = t->codec;
    if (codec == ZMS_CODEC_INVALID && t->type == ZMS_TRACK_AUDIO) {
        codec = ZMS_CODEC_AAC;
    }
    if (codec == ZMS_CODEC_INVALID) {
        return -1;
    }
    clock_hz = t->sample_rate > 0 ? t->sample_rate : (t->type == ZMS_TRACK_VIDEO ? 90000 : 44100);
    if (codec == ZMS_CODEC_AAC) {
        (void)zms_live_ingest_ensure_aac_config(rs->pb.ingress, clock_hz, rs->pb.audio_channels);
        zms_live_ingest_mark_real_audio(rs->pb.ingress);
        rs->pb.audio_rate = clock_hz;
        rs->pb.audio_clock_hz = (uint32_t)clock_hz;
    } else if (codec == ZMS_CODEC_G711A || codec == ZMS_CODEC_G711U || codec == ZMS_CODEC_OPUS) {
        rs->pb.audio_rate = clock_hz;
        rs->pb.audio_clock_hz = (uint32_t)clock_hz;
        rs->pb.audio_codec = codec;
    }
    return zms_zm_rtp_ingest_track((zms_zm_rtp_ingest *)rs->pb.zm_rtp, track_idx, codec,
                                    t->payload_type, clock_hz, NULL);
}

void zms_rtsp_session_record_input_rtp_raw(zms_rtsp_session *rs, int track_idx, const uint8_t *data,
                                           size_t len)
{
    if (!rs || track_idx < 0 || !data || len < 12) {
        return;
    }
    if (rs->destroy_scheduled) {
        return;
    }
    if (!rs->pb.zm_rtp) {
        unsigned n = rs->pb.publish_sdp.track_count ? rs->pb.publish_sdp.track_count : 2;
        int jitter_ms = rs->rtp_mode == ZMS_RTSP_RTP_UDP ? ZMS_RTP_JITTER_MS_UDP_DEFAULT
                                                         : ZMS_RTP_JITTER_MS_TCP_DEFAULT;
        rs->pb.zm_rtp = zms_zm_rtp_ingest_create((int)n, jitter_ms, rtsp_zm_on_packet, rs);
    }
    if (!rs->pb.zm_rtp || rtsp_zm_ensure_track(rs, track_idx) != 0) {
        return;
    }
    (void)zms_zm_rtp_ingest_input((zms_zm_rtp_ingest *)rs->pb.zm_rtp, track_idx, data, len);
}

void zms_rtsp_session_record_payload_teardown(zms_rtsp_session *rs)
{
    if (!rs) {
        return;
    }
    if (rs->pb.zm_rtp) {
        zms_zm_rtp_ingest_destroy((zms_zm_rtp_ingest *)rs->pb.zm_rtp);
        rs->pb.zm_rtp = NULL;
    }
}

static void rtsp_record_send_ok(zms_rtsp_session *rs)
{
    char extra[512];

    snprintf(extra, sizeof(extra),
             "Session: %s;timeout=60\r\n"
             "Range: npt=0.000-\r\n",
             rs->session_id);
    zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
}

/* 接入发布源并回 200。 */
static void rtsp_record_apply_auth(zms_rtsp_session *rs, int enable_mp4)
{
    zms_session_publish_opts pubcfg;

    rs->source->enable_mp4 = enable_mp4;
    memset(&pubcfg, 0, sizeof(pubcfg));
    pubcfg.schema = ZMS_SESSION_RTSP;
    if (zms_session_attach_publish(ZMS_SESSION_RTSP, rs, rs->source, &pubcfg) != ZN_OK) {
        zn_warn("RTSP RECORD attach publish failed: app=%s stream=%s", rs->app, rs->stream);
        zms_rtsp_session_send_resp(rs, 500, "Internal Server Error", NULL, NULL, 0);
        return;
    }
    zms_media_event_publish(rs->source, ZMS_ORIGIN_RTSP_PUSH);
    zms_media_source_set_publisher(rs->source, rs, zms_rtsp_session_publisher_kick);
    if (rs->source->enable_mp4) {
        zn_poller *pol = rs->tcp ? zn_tcp_session_poller(rs->tcp) : NULL;
        (void)zms_mp4_recorder_start(rs->source, pol);
    }
    rs->mode = ZMS_RTSP_SESSION_MODE_RECORD;
    zn_info("RTSP RECORD 200: app=%s stream=%s tracks=%u transport=%s", rs->app, rs->stream,
             rs->pb.publish_sdp.track_count, rs->rtp_mode == ZMS_RTSP_RTP_UDP ? "udp" : "tcp");
    rtsp_record_send_ok(rs);
}

void zms_rtsp_session_handle_record(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    const char *sid;
    zms_auth_request req;
    zms_auth_decision dec;
    char ip[64];
    unsigned port = 0;

    if (!rs || !msg) {
        return;
    }
    sid = zms_rtsp_message_get(msg, "Session");
    if (!rs->pb.ingress || !rs->source || !rs->source->gop_queue) {
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        return;
    }
    if (sid && !zms_rtsp_session_match(rs, sid)) {
        zms_rtsp_session_send_resp(rs, 454, "Session Not Found", NULL, NULL, 0);
        return;
    }
    if (!zms_rtsp_session_record_any_track_setup(rs)) {
        zms_rtsp_session_send_resp(rs, 455, "Method Not Valid in This State", NULL, NULL, 0);
        return;
    }
    rs->pb.logged_record = 0;
    rs->pb.logged_record_h264 = 0;
    if (rs->rtp_mode != ZMS_RTSP_RTP_UDP) {
        zms_rtsp_splitter_enable_rtp(rs->splitter, 1);
    }

    /* 推流鉴权：上游签发的 token，本地同步验签；拒绝回 403。 */
    zms_auth_fill_peer(rs->tcp, ip, sizeof(ip), &port);
    memset(&req, 0, sizeof(req));
    req.action = ZMS_AUTH_ACTION_PUBLISH;
    req.schema = ZMS_SCHEMA_RTSP;
    req.vhost = rs->vhost;
    req.app = rs->app;
    req.stream = rs->stream;
    req.params = rs->params[0] ? rs->params : NULL;
    req.ip = ip[0] ? ip : NULL;
    req.port = port;
    if (!zms_auth_check(&req, &dec)) {
        zn_warn("RTSP RECORD denied: app=%s stream=%s", rs->app, rs->stream);
        zms_rtsp_session_send_resp(rs, 403, "Forbidden", NULL, NULL, 0);
        return;
    }
    rtsp_record_apply_auth(rs, dec.enable_mp4);
}
