/**
 * @file session_setup.c
 * @brief RTSP SETUP：Transport 协商与 TCP/UDP 传输建立。
 */
#include "session/rtsp/session_internal.h"
#include "zms/session/rtsp/session_auth.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zms_rtsp_session_handle_setup(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zms_rtsp_auth_check(rs, msg)) {
        return;
    }

    const char *transport = zms_rtsp_message_get(msg, "Transport");
    const int is_push = (rs->pb.ingress != NULL);
    const int req_mcast = transport && strstr(transport, "multicast") != NULL;
    {
        int policy = zms_rtsp_rtp_transport_type();
        int req_tcp = transport && (strstr(transport, "interleaved") ||
                                    strstr(transport, "/TCP") || strstr(transport, "/tcp"));
        int req_udp = transport && (strstr(transport, "client_port") ||
                                    strstr(transport, "server_port") ||
                                    strstr(transport, "/UDP") || strstr(transport, "/udp"));
        if (policy == 2 && is_push) {
            zn_info("RTSP SETUP 461 (multicast push unsupported): url=%s", zms_rtsp_message_url(msg));
            zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
            return;
        }
        if (req_mcast && is_push) {
            zn_info("RTSP SETUP 461 (multicast RECORD unsupported): url=%s", zms_rtsp_message_url(msg));
            zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
            return;
        }
        if (!req_mcast && policy == 0 && req_udp && !req_tcp) {
            zn_info("RTSP SETUP 461 (rtpTransportType=tcp): url=%s", zms_rtsp_message_url(msg));
            zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
            return;
        }
        if (!req_mcast && policy == 1 && req_tcp && !req_udp) {
            zn_info("RTSP SETUP 461 (rtpTransportType=udp): url=%s", zms_rtsp_message_url(msg));
            zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
            return;
        }
    }
    if (zms_rtsp_test_reject_tcp_setup() && rs->pb.ingress == NULL && transport &&
        strstr(transport, "interleaved") && !strstr(transport, "UDP") &&
        !strstr(transport, "udp")) {
        zn_info("RTSP SETUP 461 (test_reject_tcp_setup): url=%s", zms_rtsp_message_url(msg));
        zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
        return;
    }
    if (!rs->session_id[0]) {
        snprintf(rs->session_id, sizeof(rs->session_id), "%u", (unsigned)rand());
    }

    int tid = zms_rtsp_session_setup_track_id(zms_rtsp_message_url(msg), rs->pb.ingress ? &rs->pb.publish_sdp : NULL);
    if (tid < 0) {
        tid = 0;
    }

    {
        int policy = zms_rtsp_rtp_transport_type();
        if (!is_push && policy == 2 && !req_mcast) {
            zn_info("RTSP SETUP 461 (rtpTransportType=multicast): url=%s", zms_rtsp_message_url(msg));
            zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
            return;
        }
        if (!is_push && req_mcast) {
            char extra[384];
            if (!zms_rtsp_multicast_enabled()) {
                zn_info("RTSP SETUP 461 (multicast disabled): url=%s", zms_rtsp_message_url(msg));
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
            if (zms_rtsp_session_setup_multicast_play(rs, tid, extra, sizeof(extra)) != 0) {
                zn_info("RTSP SETUP 461 (multicast setup failed): url=%s", zms_rtsp_message_url(msg));
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
            if (tid == 0) {
                rs->pl.video_setup = 1;
            } else {
                rs->pl.audio_setup = 1;
            }
            zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
            return;
        }
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_TCP && transport) {
        rs->rtp_mode = zms_rtsp_transport_parse_mode(transport);
    }

    if (rs->rtp_mode == ZMS_RTSP_RTP_UDP) {
        char extra[384];
        if (is_push) {
            if ((unsigned)tid >= rs->pb.publish_sdp.track_count) {
                zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                return;
            }
            rs->pb.publish_track_setup[tid] = 1;
            if (zms_rtsp_session_setup_udp_record(rs, tid, transport, extra, sizeof(extra)) != 0) {
                rs->pb.publish_track_setup[tid] = 0;
                zn_warn("RTSP SETUP UDP RECORD failed: track=%d url=%s", tid, zms_rtsp_message_url(msg));
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
        } else {
            if (zms_rtsp_session_setup_udp_play(rs, tid, transport, extra, sizeof(extra)) != 0) {
                zn_warn("RTSP SETUP UDP PLAY failed: track=%d url=%s", tid, zms_rtsp_message_url(msg));
                zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
                return;
            }
            if (tid == 0) {
                rs->pl.video_setup = 1;
            } else {
                rs->pl.audio_setup = 1;
            }
        }
        zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
        return;
    }

    if (transport && !strstr(transport, "TCP") && !strstr(transport, "tcp") &&
        !strstr(transport, "interleaved")) {
        zn_warn("RTSP SETUP: unsupported transport (%s)", transport);
        zms_rtsp_session_send_resp(rs, 461, "Unsupported Transport", NULL, NULL, 0);
        return;
    }

    uint8_t ich = (uint8_t)(tid * 2);

    if (rs->pb.ingress && rs->pb.publish_sdp.track_count > 0) {
        if ((unsigned)tid >= rs->pb.publish_sdp.track_count) {
            zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
            return;
        }
        rs->pb.publish_sdp.tracks[tid].interleaved_rtp = ich;
        rs->pb.publish_sdp.tracks[tid].interleaved_rtcp = (uint8_t)(ich + 1);
        rs->pb.publish_track_setup[tid] = 1;
        if (rs->pb.publish_sdp.tracks[tid].type == ZMS_TRACK_VIDEO) {
            rs->video_rtp_ch = ich;
            rs->video_rtcp_ch = (uint8_t)(ich + 1);
            if (!is_push) {
                rs->pl.video_setup = 1;
            }
        } else if (rs->pb.publish_sdp.tracks[tid].type == ZMS_TRACK_AUDIO) {
            rs->audio_rtp_ch = ich;
            rs->audio_rtcp_ch = (uint8_t)(ich + 1);
            if (!is_push) {
                rs->pl.audio_setup = 1;
            }
        }
    } else {
        if (tid == 0) {
            rs->video_rtp_ch = ich;
            rs->video_rtcp_ch = (uint8_t)(ich + 1);
            rs->pl.video_setup = 1;
        } else {
            rs->audio_rtp_ch = ich;
            rs->audio_rtcp_ch = (uint8_t)(ich + 1);
            rs->pl.audio_setup = 1;
        }
    }

    char extra[256];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n"
             "Session: %s;timeout=60\r\n",
             (unsigned)ich, (unsigned)(ich + 1), rs->session_id);
    zn_info("RTSP SETUP 200 TCP: track=%d interleaved=%u-%u session=%s push=%d url=%s", tid,
             (unsigned)ich, (unsigned)(ich + 1), rs->session_id, is_push ? 1 : 0, zms_rtsp_message_url(msg));
    zms_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
}
