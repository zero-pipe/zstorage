/**
 * @file session_rtcp.c
 * @brief RTSP RTCP：SR/RR 调度与统计。
 */
#include "session/rtsp/session_internal.h"
#include "zcs/media/wire/rtcp.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <string.h>

/** 双轨点播：音视频 SR 分轨发送，避免只发视频 SR 导致 VLC 长时间无音频 */
void zcs_rtsp_session_send_rtcp_sr_tracks(zcs_rtsp_session *rs, int send_video, int send_audio)
{
    const zcs_rtp_muxer_stats *st;
    const zcs_egress_clock *clk;
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t vhz;
    uint32_t ahz;
    uint8_t rtcp[64];
    size_t n;

    if (!rs || rs->mode != ZCS_RTSP_SESSION_MODE_PLAY || (!send_video && !send_audio)) {
        return;
    }
    st = zcs_rtp_muxer_get_stats(rs->pl.rtp_muxer);
    clk = zcs_rtp_muxer_play_clock(rs->pl.rtp_muxer);
    if (!st) {
        return;
    }
    vhz = rs->pb.video_clock_hz > 0 ? rs->pb.video_clock_hz : 90000u;
    ahz = rs->pb.audio_clock_hz > 0 ? rs->pb.audio_clock_hz
                                 : (uint32_t)(rs->pb.audio_rate > 0 ? rs->pb.audio_rate : 44100);
    if (send_video && rs->source && rs->source->has_video && st->video_pkt_count > 0) {
        zcs_egress_clock_sr_ntp(clk, st->video_last_rtp_ts, vhz, &ntp_sec, &ntp_frac);
        n = zcs_rtcp_build_sr(rtcp, sizeof(rtcp), rs->pl.video_rtp_ssrc, ntp_sec, ntp_frac,
                              st->video_last_rtp_ts, st->video_pkt_count, st->video_octet_count);
        if (n) {
            zcs_rtsp_session_send_media(rs, rs->video_rtcp_ch, rtcp, n);
        }
    }
    if (send_audio && rs->source && rs->source->has_audio && st->audio_pkt_count > 0) {
        zcs_egress_clock_sr_ntp(clk, st->audio_last_rtp_ts, ahz, &ntp_sec, &ntp_frac);
        n = zcs_rtcp_build_sr(rtcp, sizeof(rtcp), rs->pl.audio_rtp_ssrc, ntp_sec, ntp_frac,
                              st->audio_last_rtp_ts, st->audio_pkt_count, st->audio_octet_count);
        if (n) {
            zcs_rtsp_session_send_media(rs, rs->audio_rtcp_ch, rtcp, n);
        }
    }
}

void zcs_rtsp_session_play_try_rtcp_boot(zcs_rtsp_session *rs)
{
    const zcs_rtp_muxer_stats *st;
    int sent = 0;

    if (!rs || !rs->pl.rtp_muxer || !rs->source) {
        return;
    }
    if (rs->pl.vod_reader && zcs_rtp_muxer_catchup_on(rs->pl.rtp_muxer)) {
        return;
    }
    st = zcs_rtp_muxer_get_stats(rs->pl.rtp_muxer);
    if (!st) {
        return;
    }
    if (rs->source->has_video && !rs->pl.rtcp_video_sr_sent && st->video_pkt_count > 0) {
        zcs_rtsp_session_send_rtcp_sr_tracks(rs, 1, 0);
        rs->pl.rtcp_video_sr_sent = 1;
        sent = 1;
    }
    if (rs->source->has_audio && !rs->pl.rtcp_audio_sr_sent && st->audio_pkt_count > 0) {
        zcs_rtsp_session_send_rtcp_sr_tracks(rs, 0, 1);
        rs->pl.rtcp_audio_sr_sent = 1;
        sent = 1;
    }
    if ((!rs->source->has_video || rs->pl.rtcp_video_sr_sent) &&
        (!rs->source->has_audio || rs->pl.rtcp_audio_sr_sent)) {
        rs->pl.rtcp_boot_sent = 1;
    }
    if (sent && rs->tcp) {
        zn_tcp_session_flush(rs->tcp);
    }
}

void zcs_rtsp_session_send_rtcp_srs(zcs_rtsp_session *rs)
{
    zcs_rtsp_session_send_rtcp_sr_tracks(rs, 1, 1);
}

void zcs_rtsp_session_play_on_rtcp(zcs_rtsp_session *rs, const uint8_t *data, size_t len)
{
    zcs_rtcp_rr rr;
    if (zcs_rtcp_parse_rr(data, len, &rr) == ZN_OK) {
        (void)rr;
    }
}
